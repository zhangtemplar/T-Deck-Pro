/**
 * @file      map_file.c
 * @brief     .tdmap reader. See map_file.h and tools/gen_map.py.
 */
#include "map_file.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* File access is abstracted so the whole reader can be exercised on the host
 * against a file written by gen_map.py, which is where the format bugs are
 * cheapest to find. On the device it goes through SD with the shared SPI lock
 * held, because the display sits on the same bus. */
#ifdef ARDUINO
#include <Arduino.h>
#include <SD.h>
#include "utilities.h"
#include "peripheral.h"

/* Declared the way the other SD users in this tree do it — there is no header
 * for the shared bus lock, it lives in ui_deckpro_port.cpp. */
extern void shared_spi_lock(void);
extern void shared_spi_unlock(void);
extern void shared_spi_prepare_device(int cs_pin);

typedef File map_fh_t;
static map_fh_t s_fh;
static bool     s_open = false;

static bool fh_open(const char *path)
{
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    s_fh = SD.open(path, FILE_READ);
    bool ok = (bool)s_fh;
    shared_spi_unlock();
    return ok;
}
static void fh_close(void)   { if (s_open) { shared_spi_lock(); s_fh.close(); shared_spi_unlock(); } }
static bool fh_read(uint32_t off, void *dst, uint32_t len)
{
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    bool ok = s_fh.seek(off) && s_fh.read((uint8_t *)dst, len) == (int)len;
    shared_spi_unlock();
    return ok;
}
#define MAP_ALLOC(n) ps_malloc(n)
#define MAP_LOG(...)  Serial.printf(__VA_ARGS__)

#else   /* host */
static FILE *s_fp = NULL;
static bool  s_open = false;
static bool fh_open(const char *path) { s_fp = fopen(path, "rb"); return s_fp != NULL; }
static void fh_close(void) { if (s_fp) { fclose(s_fp); s_fp = NULL; } }
static bool fh_read(uint32_t off, void *dst, uint32_t len)
{
    return s_fp && fseek(s_fp, (long)off, SEEK_SET) == 0 && fread(dst, 1, len, s_fp) == len;
}
#define MAP_ALLOC(n) malloc(n)
#define MAP_LOG(...)  printf(__VA_ARGS__)
#endif

#define MAP_MAX_LEVELS 8

typedef struct {
    int32_t  span;         /* degrees x 1e7 */
    uint16_t n_cols, n_rows;
    uint32_t index_off;
} level_t;

static int32_t s_min_lat, s_min_lon, s_max_lat, s_max_lon;
static int     s_n_levels;
static level_t s_levels[MAP_MAX_LEVELS];

/* One tile's raw bytes and one feature's decoded points. Allocated once at
 * open rather than per query: a query runs on every pan, and PSRAM allocation
 * is not free. */
static uint8_t *s_tile_buf;
static uint32_t s_tile_cap;
static int32_t *s_lat_buf, *s_lon_buf;

#define TILE_BUF_CAP (48u * 1024u)

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static int32_t  rd_i32(const uint8_t *p) { return (int32_t)rd_u32(p); }

void map_close(void)
{
    if (!s_open) return;
    fh_close();
    free(s_tile_buf); s_tile_buf = NULL;
    free(s_lat_buf);  s_lat_buf = NULL;
    free(s_lon_buf);  s_lon_buf = NULL;
    s_n_levels = 0;
    s_open = false;
}

bool map_is_open(void) { return s_open; }

bool map_open(const char *path)
{
    map_close();
    if (!fh_open(path)) {
        MAP_LOG("[MAP] cannot open %s\n", path);
        return false;
    }
    s_open = true;                       /* so fh_close() in map_close() runs */

    uint8_t hdr[32];
    if (!fh_read(0, hdr, sizeof(hdr)) || memcmp(hdr, "TDMAP\0", 6) != 0) {
        MAP_LOG("[MAP] %s is not a .tdmap\n", path);
        map_close();
        return false;
    }
    if (hdr[6] != 1) {
        MAP_LOG("[MAP] %s is version %u, this build reads 1\n", path, hdr[6]);
        map_close();
        return false;
    }

    s_n_levels = hdr[7];
    if (s_n_levels <= 0 || s_n_levels > MAP_MAX_LEVELS) {
        MAP_LOG("[MAP] %d levels is out of range\n", s_n_levels);
        map_close();
        return false;
    }

    s_min_lat = rd_i32(hdr + 8);
    s_min_lon = rd_i32(hdr + 12);
    s_max_lat = rd_i32(hdr + 16);
    s_max_lon = rd_i32(hdr + 20);
    uint32_t ltab = rd_u32(hdr + 24);

    uint8_t lt[MAP_MAX_LEVELS * 16];
    if (!fh_read(ltab, lt, (uint32_t)s_n_levels * 16)) {
        MAP_LOG("[MAP] truncated level table\n");
        map_close();
        return false;
    }
    for (int i = 0; i < s_n_levels; i++) {
        const uint8_t *e = lt + i * 16;
        s_levels[i].span      = rd_i32(e);
        s_levels[i].n_cols    = rd_u16(e + 4);
        s_levels[i].n_rows    = rd_u16(e + 6);
        s_levels[i].index_off = rd_u32(e + 8);
        if (s_levels[i].span <= 0 || s_levels[i].n_cols == 0 || s_levels[i].n_rows == 0) {
            MAP_LOG("[MAP] level %d is malformed\n", i);
            map_close();
            return false;
        }
    }

    s_tile_cap = TILE_BUF_CAP;
    s_tile_buf = (uint8_t *)MAP_ALLOC(s_tile_cap);
    s_lat_buf  = (int32_t *)MAP_ALLOC(MAP_MAX_POINTS * sizeof(int32_t));
    s_lon_buf  = (int32_t *)MAP_ALLOC(MAP_MAX_POINTS * sizeof(int32_t));
    if (!s_tile_buf || !s_lat_buf || !s_lon_buf) {
        MAP_LOG("[MAP] out of memory for tile buffers\n");
        map_close();
        return false;
    }

    MAP_LOG("[MAP] %s: %d levels, lat %.4f..%.4f lon %.4f..%.4f\n",
            path, s_n_levels,
            s_min_lat / 1e7, s_max_lat / 1e7, s_min_lon / 1e7, s_max_lon / 1e7);
    return true;
}

void map_bounds(int32_t *min_lat, int32_t *min_lon, int32_t *max_lat, int32_t *max_lon)
{
    if (min_lat) *min_lat = s_min_lat;
    if (min_lon) *min_lon = s_min_lon;
    if (max_lat) *max_lat = s_max_lat;
    if (max_lon) *max_lon = s_max_lon;
}

int map_level_count(void) { return s_n_levels; }

int32_t map_level_span(int level)
{
    if (!s_open || level < 0 || level >= s_n_levels) return 0;
    return s_levels[level].span;
}

int map_pick_level(int32_t lat_span, int px)
{
    if (!s_open) return 0;
    if (px <= 0) px = 240;
    if (lat_span <= 0) lat_span = 1;

    /* The writer simplified each level to about span/240 of a degree, so a
     * level is "enough detail" when its tile span is no larger than the
     * viewport. Walk from coarse to fine and stop at the last one that still
     * covers the view in a handful of tiles — going finer only multiplies the
     * number of SD reads without adding anything the panel can show. */
    int best = 0;
    for (int i = 0; i < s_n_levels; i++) {
        if (s_levels[i].span * 4 >= lat_span) best = i;
    }
    return best;
}

/* Zigzag varint. Returns the number of bytes consumed, or 0 if the buffer
 * ends mid-value or the value is implausibly long. */
static int rd_varint(const uint8_t *p, uint32_t avail, int32_t *out)
{
    uint32_t v = 0;
    int shift = 0;
    for (uint32_t i = 0; i < avail && i < 5; i++) {
        v |= (uint32_t)(p[i] & 0x7F) << shift;
        if (!(p[i] & 0x80)) {
            *out = (int32_t)((v >> 1) ^ (uint32_t)(-(int32_t)(v & 1)));
            return (int)i + 1;
        }
        shift += 7;
    }
    return 0;
}

/* Decode one tile's features and hand them to the callback. */
static int emit_tile(const uint8_t *buf, uint32_t len, int level,
                     uint16_t col, uint16_t row, map_feature_cb cb, void *user)
{
    const level_t *L = &s_levels[level];
    /* One local unit is span/65536 of a degree. Kept as a rational rather
     * than a float so the arithmetic is exact and identical to the writer's. */
    const int32_t t_lat0 = s_min_lat + (int32_t)((int64_t)L->span * row);
    const int32_t t_lon0 = s_min_lon + (int32_t)((int64_t)L->span * col);

    uint32_t p = 0;
    int n_feat = 0;

    while (p + 4 <= len) {
        uint8_t  type  = buf[p];
        uint8_t  flags = buf[p + 1];
        uint16_t n     = rd_u16(buf + p + 2);
        p += 4;

        if (n == 0 || n > MAP_MAX_POINTS) return n_feat;   /* corrupt; stop here */

        int32_t lx = 0, ly = 0;
        bool ok = true;
        for (uint16_t i = 0; i < n; i++) {
            int32_t dy, dx;
            int a = rd_varint(buf + p, len - p, &dy);
            if (!a) { ok = false; break; }
            p += (uint32_t)a;
            int b = rd_varint(buf + p, len - p, &dx);
            if (!b) { ok = false; break; }
            p += (uint32_t)b;

            ly += dy;
            lx += dx;
            s_lat_buf[i] = t_lat0 + (int32_t)(((int64_t)ly * L->span) >> 16);
            s_lon_buf[i] = t_lon0 + (int32_t)(((int64_t)lx * L->span) >> 16);
        }
        if (!ok) return n_feat;

        if (type < MAP_TYPE_COUNT && cb) cb(type, flags, s_lat_buf, s_lon_buf, n, user);
        n_feat++;
    }
    return n_feat;
}

int map_query(int level,
              int32_t min_lat, int32_t min_lon,
              int32_t max_lat, int32_t max_lon,
              map_feature_cb cb, void *user)
{
    if (!s_open || level < 0 || level >= s_n_levels) return -1;
    const level_t *L = &s_levels[level];

    if (min_lat > max_lat) { int32_t t = min_lat; min_lat = max_lat; max_lat = t; }
    if (min_lon > max_lon) { int32_t t = min_lon; min_lon = max_lon; max_lon = t; }

    /* Viewport -> tile range, clamped to the grid. */
    int64_t c0 = ((int64_t)min_lon - s_min_lon) / L->span;
    int64_t c1 = ((int64_t)max_lon - s_min_lon) / L->span;
    int64_t r0 = ((int64_t)min_lat - s_min_lat) / L->span;
    int64_t r1 = ((int64_t)max_lat - s_min_lat) / L->span;
    if (c0 < 0) c0 = 0;
    if (r0 < 0) r0 = 0;
    if (c1 > L->n_cols - 1) c1 = L->n_cols - 1;
    if (r1 > L->n_rows - 1) r1 = L->n_rows - 1;
    if (c1 < c0 || r1 < r0) return 0;

    int total = 0;
    for (int64_t row = r0; row <= r1; row++) {
        for (int64_t col = c0; col <= c1; col++) {
            uint32_t slot = (uint32_t)(row * L->n_cols + col);
            uint8_t ent[8];
            if (!fh_read(L->index_off + slot * 8, ent, 8)) continue;

            uint32_t off = rd_u32(ent);
            uint32_t len = rd_u32(ent + 4);
            if (off == 0 || len == 0) continue;              /* empty tile */
            if (len > s_tile_cap) {
                MAP_LOG("[MAP] tile %u is %u bytes, buffer is %u; skipped\n",
                        (unsigned)slot, (unsigned)len, (unsigned)s_tile_cap);
                continue;
            }
            if (!fh_read(off, s_tile_buf, len)) continue;

            total += emit_tile(s_tile_buf, len, level,
                               (uint16_t)col, (uint16_t)row, cb, user);
        }
    }
    return total;
}
