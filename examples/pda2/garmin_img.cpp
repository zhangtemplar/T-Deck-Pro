/**
 * @file      garmin_img.cpp
 * @brief     Classic Garmin .img reader. See garmin_img.h.
 *
 * The bitstream decoder is ported from org.free.garminimg by way of the Dart
 * port at github.com/vasilevzhivko/garmin_img, including the escape case in
 * read_coord_offset() that a plain two's-complement read gets wrong.
 *
 * ---- Why the index is shaped the way it is -------------------------------
 *
 * A country-sized gmapsupp.img runs to gigabytes: a few thousand map tiles and
 * millions of subdivisions. Two facts of this platform shape everything here.
 *
 * First, subdivisions cannot be resident. Millions of them at 20 bytes each
 * would be tens of megabytes against 8 MB of PSRAM. So the index holds only
 * per-tile information — bounds, where its sections live, its level table —
 * and a query reads the one contiguous run of subdivision records belonging to
 * the level it needs, then throws it away.
 *
 * Second, and worse: ESP-IDF ships FatFs with CONFIG_FATFS_USE_FASTSEEK off,
 * and the prebuilt library has no cltbl field to turn it on with. f_lseek
 * therefore walks the FAT cluster chain. Seeking forward continues from the
 * current cluster and is cheap for a short hop; seeking BACKWARD restarts from
 * the file's first cluster, which on a 2 GB file with 32 KB clusters is some
 * 70,000 chain entries — most of a second on a 4 MHz bus, every time.
 *
 * So this reader goes to some trouble never to seek backwards when it can help
 * it: a query gathers the subdivisions it needs, sorts them by file offset, and
 * then decodes in that order. Building the tile index likewise walks the file
 * once, forwards, and caches the result in a sidecar so it is paid once per
 * card rather than once per app launch.
 */
#include "garmin_img.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <SD.h>
#include "utilities.h"

extern void shared_spi_lock(void);
extern void shared_spi_unlock(void);
extern void shared_spi_prepare_device(int cs_pin);
extern bool sd_ensure_mounted(void);

static File s_fh;
static bool fh_open(const char *p)
{
    if (!sd_ensure_mounted()) return false;
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    s_fh = SD.open(p, FILE_READ);
    bool ok = (bool)s_fh;
    shared_spi_unlock();
    return ok;
}
static void fh_close(void) { shared_spi_lock(); s_fh.close(); shared_spi_unlock(); }
static bool fh_read(uint64_t off, void *dst, uint32_t len)
{
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    bool ok = s_fh.seek((uint32_t)off) && s_fh.read((uint8_t *)dst, len) == (int)len;
    shared_spi_unlock();
    return ok;
}
static uint64_t fh_size(void) { return (uint64_t)s_fh.size(); }

static bool sidecar_write(const char *path, const void *a, uint32_t na,
                          const void *b, uint32_t nb, const void *c, uint32_t nc,
                          const void *d, uint32_t nd)
{
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    File f = SD.open(path, FILE_WRITE);
    bool ok = false;
    if (f) {
        ok = f.write((const uint8_t *)a, na) == na &&
             f.write((const uint8_t *)b, nb) == nb &&
             f.write((const uint8_t *)c, nc) == nc &&
             f.write((const uint8_t *)d, nd) == nd;
        f.close();
    }
    shared_spi_unlock();
    return ok;
}
static bool sidecar_read(const char *path, void *hdr, uint32_t nh,
                         void **a, uint32_t na, void **b, uint32_t nb,
                         void **c, uint32_t nc);
#define GALLOC(n)      ps_malloc(n)
#define GREALLOC(p, n) ps_realloc(p, n)
#define GLOG(...)      Serial.printf(__VA_ARGS__)

#else   /* host */
static FILE *s_fp;
static bool fh_open(const char *p) { s_fp = fopen(p, "rb"); return s_fp != NULL; }
static void fh_close(void) { if (s_fp) { fclose(s_fp); s_fp = NULL; } }
static bool fh_read(uint64_t off, void *dst, uint32_t len)
{
    return s_fp && fseeko(s_fp, (off_t)off, SEEK_SET) == 0 && fread(dst, 1, len, s_fp) == len;
}
static uint64_t fh_size(void)
{
    if (!s_fp) return 0;
    off_t cur = ftello(s_fp);
    fseeko(s_fp, 0, SEEK_END);
    off_t n = ftello(s_fp);
    fseeko(s_fp, cur, SEEK_SET);
    return (uint64_t)n;
}
static bool sidecar_write(const char *path, const void *a, uint32_t na,
                          const void *b, uint32_t nb, const void *c, uint32_t nc,
                          const void *d, uint32_t nd)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(a, 1, na, f) == na && fwrite(b, 1, nb, f) == nb &&
              fwrite(c, 1, nc, f) == nc && fwrite(d, 1, nd, f) == nd;
    fclose(f);
    return ok;
}
#define GALLOC(n)      malloc(n)
#define GREALLOC(p, n) realloc(p, n)
#define GLOG(...)      printf(__VA_ARGS__)
#endif

/* Optional instrumentation. Compiled out unless GIMG_STATS is defined, and
 * used to size the device's redraw budget: on SD the number of read calls
 * matters more than the number of bytes, because each one is a seek. */
#ifdef GIMG_STATS
gimg_stats_t g_gimg_stats;
#define STAT(field, n) (g_gimg_stats.field += (n))
#else
#define STAT(field, n) ((void)0)
#endif

/* ------------------------------------------------------------------ limits */
#define MAX_LEVELS      16
/* Biggest run of RGN bytes read in one go. Subdivision chunks are a few KB in
 * practice; anything wildly larger means we have misparsed. */
#define CHUNK_CAP       (64u * 1024u)
/* Subdivision records read per batch, so a tile with tens of thousands of them
 * still only needs a fixed buffer. */
#define SUBDIV_BATCH    255   /* +1 record of lookahead still fits the buffer */
/* Subdivisions decoded per query. Beyond this the panel refresh is the least
 * of our problems; the cap keeps the sort buffer fixed-size. */
#define MAX_HITS        512
/* Tiles touched by one redraw. Each costs a seek across the whole file, so
 * this is the difference between a redraw and a watchdog reset: a viewport
 * covering a whole country overlaps every tile there is. */
#define MAX_TILES_PER_Q 8
/* Read the pseudo-FAT in big sequential gulps rather than a slot at a time:
 * a few thousand tiles means ~12,000 slots, and one read call each would be
 * the single slowest thing in the whole open. */
#define FAT_BATCH       (32u * 1024u)

#define SIDECAR_MAGIC   "TDXG1\0"
#define SIDECAR_VER     1

/* ------------------------------------------------------------------ types */
/* Subfiles are stored as runs of consecutive blocks. In practice mkgmap lays
 * each one down contiguously, so this is almost always a single run and a read
 * becomes a single seek regardless of length. */
typedef struct { uint32_t start, count; } blkrun_t;

typedef struct { uint8_t zoom, bpc; uint16_t count; } lvl_t;

typedef struct {
    int32_t  north, east, south, west;   /* garmin units */
    uint32_t tre_size, rgn_size;
    uint32_t tre2_off, rgn_data0;
    uint32_t run_tre, run_rgn;           /* index into the pooled run array */
    uint16_t n_tre_runs, n_rgn_runs;
    uint32_t lvl_first;                  /* index into the pooled level array */
    uint8_t  n_levels;
    uint8_t  pad[3];
} tile_t;

typedef struct {
    uint32_t rgn_off;
    uint32_t rgn_end;      /* where the next subdivision's data starts */
    int32_t  clat, clon;
    uint16_t w, h;
    uint8_t  elem, bpc;
} subdiv_t;

static bool     s_open = false;
static uint32_t s_block_size = 512;
static uint64_t s_src_size = 0;

static tile_t  *s_tiles;  static uint32_t s_n_tiles, s_cap_tiles;
static blkrun_t*s_runs;   static uint32_t s_n_runs,  s_cap_runs;
static lvl_t   *s_lvls;   static uint32_t s_n_lvls,  s_cap_lvls;

static int32_t  s_min_lat, s_min_lon, s_max_lat, s_max_lon;
static char     s_desc[96];

static uint8_t *s_chunk;      /* one RGN chunk           */
static uint8_t *s_sdbuf;      /* one batch of subdiv recs */
static int32_t *s_lat_buf, *s_lon_buf;

/* ------------------------------------------------------------------ helpers */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int16_t rds16(const uint8_t *p) { return (int16_t)rd16(p); }
static uint32_t rd24(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}
static int32_t rds24(const uint8_t *p)
{
    uint32_t v = rd24(p);
    return (v & 0x800000u) ? (int32_t)v - 0x1000000 : (int32_t)v;
}

/* Garmin units (2^24 per 360 degrees) to degrees x 1e7. */
static int32_t gu_to_e7(int32_t gu)
{
    return (int32_t)(((int64_t)gu * 3600000000LL) >> 24);
}
static int32_t e7_to_gu(int32_t e7)
{
    /* Multiply rather than shift: e7 is routinely negative, and shifting a
     * negative value left is undefined. */
    return (int32_t)(((int64_t)e7 * 16777216LL) / 3600000000LL);
}

/* Scale a signed delta up by `sh` bits. Same reason. */
static inline int32_t shl(int32_t v, int sh) { return (int32_t)((int64_t)v * (1LL << sh)); }

/* Absolute file offset of a byte within a subfile, or UINT64_MAX past the end.
 * Also reports how many bytes remain contiguous from there, so a caller can
 * read a whole run in one go. */
static uint64_t sub_locate(const blkrun_t *runs, uint32_t n_runs,
                           uint64_t off, uint32_t *contig)
{
    for (uint32_t i = 0; i < n_runs; i++) {
        uint64_t span = (uint64_t)runs[i].count * s_block_size;
        if (off < span) {
            if (contig) *contig = (uint32_t)(span - off);
            return (uint64_t)runs[i].start * s_block_size + off;
        }
        off -= span;
    }
    return UINT64_MAX;
}

static bool runs_read(uint32_t run_first, uint32_t n_runs, uint32_t sub_size,
                      uint64_t off, void *dst, uint32_t len)
{
    if (off + len > sub_size) return false;
    uint8_t *out = (uint8_t *)dst;
    const blkrun_t *runs = s_runs + run_first;

    while (len) {
        uint32_t contig = 0;
        uint64_t abs = sub_locate(runs, n_runs, off, &contig);
        if (abs == UINT64_MAX) return false;
        uint32_t n = (contig < len) ? contig : len;
        if (!fh_read(abs, out, n)) return false;
        STAT(reads, 1);
        STAT(bytes, n);
        out += n;
        off += n;
        len -= n;
    }
    return true;
}

#define TRE_READ(t, off, dst, len) runs_read((t)->run_tre, (t)->n_tre_runs, (t)->tre_size, (off), (dst), (len))
#define RGN_READ(t, off, dst, len) runs_read((t)->run_rgn, (t)->n_rgn_runs, (t)->rgn_size, (off), (dst), (len))

static uint16_t rgn_u16(const tile_t *t, uint32_t off)
{
    uint8_t b[2];
    return RGN_READ(t, off, b, 2) ? rd16(b) : 0;
}

/* ------------------------------------------------------------------ bitstream */
typedef struct { const uint8_t *b; uint32_t nbits, pos; } bits_t;

static void bits_init(bits_t *t, const uint8_t *b, uint32_t nbytes)
{
    t->b = b; t->nbits = nbytes * 8; t->pos = 0;
}
static uint32_t bits_left(const bits_t *t) { return t->nbits - t->pos; }

/* LSB-first. Reads past the end return zeros: streams are byte-padded. */
static uint32_t bits_get(bits_t *t, int n)
{
    STAT(bits, n);
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        if (t->pos >= t->nbits) return v;
        v |= (uint32_t)((t->b[t->pos >> 3] >> (t->pos & 7)) & 1) << i;
        t->pos++;
    }
    return v;
}

/* Bits per delta: the info nibble, widened above 9, plus 2, plus one for a
 * variable sign and one for the longitude "extra" bit. */
static int coord_len(int i, int sign, int extra)
{
    int add = (sign == 0 ? 1 : 0) + extra;
    return (i <= 9 ? i : 2 * i - 9) + 2 + add;
}

/* One signed delta. The escape case — sign bit set with a zero complement,
 * meaning "read another group and combine" — is what a naive two's-complement
 * read misses, and omitting it produces occasional wild vertices. */
static int32_t read_coord_offset_d(bits_t *br, int nb, int sign, int extra, int depth);
static int32_t read_coord_offset(bits_t *br, int nb, int sign, int extra)
{
    return read_coord_offset_d(br, nb, sign, extra, 0);
}

/* `depth` bounds the escape case. A well-formed stream chains it once or twice;
 * a misparsed chunk could in principle chain until the stack gives out. */
static int32_t read_coord_offset_d(bits_t *br, int nb, int sign, int extra, int depth)
{
    if (depth > 16 || nb <= 0 || nb > 32) return 0;
    if (sign == 0) {
        uint32_t value = bits_get(br, nb);
        uint32_t smask = 1u << (nb - 1);
        if (value & smask) {
            uint32_t comp = value ^ smask;
            if (extra == 0) {
                if (comp) return (int32_t)comp - (int32_t)smask;
                int32_t other = read_coord_offset_d(br, nb, sign, extra, depth + 1);
                return other < 0 ? 1 - (int32_t)value + other
                                 : (int32_t)value - 1 + other;
            } else {
                if (comp & 0xFFFFFEu) return (int32_t)(comp & 0xFFFFFEu) - (int32_t)smask;
                int32_t other = read_coord_offset_d(br, nb - 1, sign, 0, depth + 1);
                return other < 0 ? 1 - (int32_t)smask + 1 + other * 2
                                 : (int32_t)smask - 1 - 1 + other * 2;
            }
        }
        return extra ? (int32_t)(value & 0xFFFFFEu) : (int32_t)value;
    }
    uint32_t val = bits_get(br, nb);
    return extra ? (((int32_t)val >> 1) * sign) * 2 : (int32_t)val * sign;
}


/* ------------------------------------------------------------------ decode */
/* A subdivision's RGN data runs until the next subdivision's begins. The
 * subdivision table is no longer resident, but RGN offsets ascend monotonically
 * through TRE2, so the following record supplies the bound — scan_level reads
 * it as lookahead and stores it in rgn_end. Without a real bound each chunk
 * decodes on into its neighbour's data and emits it all over again. */
static int decode_lines(const tile_t *t, const subdiv_t *sd, int want_kind,
                        gimg_feature_cb cb, void *user)
{
    const uint8_t present_bits[4] = {0x10, 0x20, 0x40, 0x80};
    int n_present = 0, idx = -1;
    for (int i = 0; i < 4; i++) {
        if (sd->elem & present_bits[i]) {
            if (i == want_kind) idx = n_present;
            n_present++;
        }
    }
    if (idx < 0) return 0;

    uint32_t base = t->rgn_data0 + sd->rgn_off;
    uint32_t hdr_len = (uint32_t)(n_present - 1) * 2;
    uint32_t start = (idx == 0) ? base + hdr_len
                                : base + rgn_u16(t, base + (uint32_t)(idx - 1) * 2);
    uint32_t end;
    if (idx + 1 < n_present) {
        end = base + rgn_u16(t, base + (uint32_t)idx * 2);
    } else {
        end = t->rgn_data0 + sd->rgn_end;
        if (end > t->rgn_size) end = t->rgn_size;
    }
    if (end <= start || start >= t->rgn_size) return 0;

    uint32_t len = end - start;
    if (len > CHUNK_CAP) len = CHUNK_CAP;
    if (!runs_read(t->run_rgn, t->n_rgn_runs, t->rgn_size, start, s_chunk, len)) return 0;

    const int32_t lim_lon = (int32_t)(sd->w ? sd->w : 0x2000) * 6 + 64;
    const int32_t lim_lat = (int32_t)(sd->h ? sd->h : 0x2000) * 6 + 64;
    const int shift = 24 - sd->bpc;

    uint32_t p = 0;
    int emitted = 0;

    while (p + 9 <= len) {
        uint8_t  type  = s_chunk[p];
        uint32_t info3 = rd24(s_chunk + p + 1);
        int lon_extra  = (info3 & 0x400000u) ? 1 : 0;
        int32_t  dlon  = rds16(s_chunk + p + 4);
        int32_t  dlat  = rds16(s_chunk + p + 6);

        uint32_t blen, bs;
        if (type & 0x80) { blen = rd16(s_chunk + p + 8); bs = p + 10; }
        else             { blen = s_chunk[p + 8];        bs = p + 9;  }

        /* The length counts data bytes only; the leading info byte is extra,
         * so the stream spans blen+1 and the next object follows it. Getting
         * this off by one desynchronises every object after the first. */
        uint32_t total = blen + 1;
        if (blen < 1 || bs + total > len) break;

        /* A real object's first point lies inside its own subdivision. A wildly
         * bigger delta means we have run past the end of this chunk — which is
         * how the unbounded case above terminates. */
        if (dlon > lim_lon || dlon < -lim_lon || dlat > lim_lat || dlat < -lim_lat) break;

        uint8_t info = s_chunk[bs];
        bits_t br;
        bits_init(&br, s_chunk + bs + 1, blen);

        int lon_sign = 0, lat_sign = 0;
        if (bits_get(&br, 1)) lon_sign = bits_get(&br, 1) == 0 ? 1 : -1;
        if (bits_get(&br, 1)) lat_sign = bits_get(&br, 1) == 0 ? 1 : -1;

        int lon_bits = coord_len(info & 0x0F, lon_sign, lon_extra);
        int lat_bits = coord_len(info >> 4,   lat_sign, 0);
        int shift_lon = shift - lon_extra;

        int n = 0;
        s_lat_buf[n] = gu_to_e7(sd->clat + shl(dlat, shift));
        s_lon_buf[n] = gu_to_e7(sd->clon + shl(dlon, shift));
        n++;

        int32_t clon = shl(dlon, lon_extra), clat = dlat;
        while (bits_left(&br) >= (uint32_t)(lon_bits + lat_bits)) {
            clon += read_coord_offset(&br, lon_bits, lon_sign, lon_extra);
            clat += read_coord_offset(&br, lat_bits, lat_sign, 0);

            STAT(points, 1);
            s_lat_buf[n] = gu_to_e7(sd->clat + shl(clat, shift));
            s_lon_buf[n] = gu_to_e7(sd->clon + (shift_lon >= 0 ? shl(clon, shift_lon)
                                                               : (clon >> -shift_lon)));
            n++;

            if (n == GIMG_MAX_POINTS) {
                /* Emit and carry the last point over, so the split is
                 * invisible once drawn. */
                if (cb) cb(type & 0x3F, (uint8_t)want_kind, s_lat_buf, s_lon_buf, n, user);
                emitted++;
                s_lat_buf[0] = s_lat_buf[n - 1];
                s_lon_buf[0] = s_lon_buf[n - 1];
                n = 1;
            }
        }

        if (n > 1 && cb) {
            cb(type & 0x3F, (uint8_t)want_kind, s_lat_buf, s_lon_buf, n, user);
            emitted++;
        }
        p = bs + total;
    }
    return emitted;
}

/* ------------------------------------------------------------------ index */
static void *grow(void *p, uint32_t *cap, uint32_t need, uint32_t esz)
{
    if (need <= *cap) return p;
    uint32_t nc = *cap ? *cap * 2 : 64;
    while (nc < need) nc *= 2;
    void *np = GREALLOC(p, (size_t)nc * esz);
    if (!np) return NULL;
    *cap = nc;
    return np;
}

void gimg_close(void)
{
    if (!s_open) return;
    free(s_tiles); s_tiles = NULL; s_n_tiles = s_cap_tiles = 0;
    free(s_runs);  s_runs  = NULL; s_n_runs  = s_cap_runs  = 0;
    free(s_lvls);  s_lvls  = NULL; s_n_lvls  = s_cap_lvls  = 0;
    free(s_chunk); s_chunk = NULL;
    free(s_sdbuf); s_sdbuf = NULL;
    free(s_lat_buf); s_lat_buf = NULL;
    free(s_lon_buf); s_lon_buf = NULL;
    fh_close();
    s_open = false;
}

bool gimg_is_open(void) { return s_open; }

/* Read a tile's TRE header: bounds, level table, and where TRE2 begins.
 * Deliberately does NOT touch the subdivisions — that is the whole point of
 * the lazy index. The header and the level table are pulled in one read
 * because they sit within a few hundred bytes of each other and a second read
 * would be a second seek. */
static bool index_tile_tre(tile_t *t)
{
    uint8_t head[1024];
    uint32_t want = t->tre_size < sizeof(head) ? t->tre_size : (uint32_t)sizeof(head);
    if (want < 0x30 || !TRE_READ(t, 0, head, want)) return false;
    if (memcmp(head + 2, "GARMIN TRE", 10) != 0) return false;

    t->north = rds24(head + 0x15);
    t->east  = rds24(head + 0x18);
    t->south = rds24(head + 0x1B);
    t->west  = rds24(head + 0x1E);

    uint32_t tre1 = rd32(head + 0x21), tre1_sz = rd32(head + 0x25);
    t->tre2_off = rd32(head + 0x29);

    int n = (int)(tre1_sz / 4);
    if (n <= 0 || n > MAX_LEVELS) return false;

    uint8_t lt[MAX_LEVELS * 4];
    const uint8_t *src;
    if (tre1 + (uint32_t)n * 4 <= want) {
        src = head + tre1;                       /* already in hand */
    } else {
        if (!TRE_READ(t, tre1, lt, (uint32_t)n * 4)) return false;
        src = lt;
    }

    s_lvls = (lvl_t *)grow(s_lvls, &s_cap_lvls, s_n_lvls + (uint32_t)n, sizeof(lvl_t));
    if (!s_lvls) return false;
    t->lvl_first = s_n_lvls;
    t->n_levels  = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        s_lvls[s_n_lvls + i].zoom  = src[i * 4] & 0x0F;
        s_lvls[s_n_lvls + i].bpc   = src[i * 4 + 1];
        s_lvls[s_n_lvls + i].count = rd16(src + i * 4 + 2);
    }
    s_n_lvls += (uint32_t)n;
    return true;
}

static bool index_tile_rgn(tile_t *t)
{
    uint8_t rhdr[0x19];
    if (!RGN_READ(t, 0, rhdr, sizeof(rhdr))) return false;
    if (memcmp(rhdr + 2, "GARMIN RGN", 10) != 0) return false;
    t->rgn_data0 = rd32(rhdr + 0x15);
    return true;
}

/* Byte offset of level `lv`'s subdivision records within TRE2, and its stride.
 * Every level uses 16-byte records except the finest, which drops the trailing
 * child index and uses 14. */
static uint32_t level_rec_off(const tile_t *t, int lv, uint32_t *stride_out)
{
    uint32_t off = 0;
    for (int i = 0; i < lv; i++) {
        uint32_t stride = (i == t->n_levels - 1) ? 14 : 16;
        off += (uint32_t)s_lvls[t->lvl_first + i].count * stride;
    }
    if (stride_out) *stride_out = (lv == t->n_levels - 1) ? 14 : 16;
    return off;
}

/* RGN offset that bounds the LAST subdivision of `level`: the first
 * subdivision of the next level, or the end of RGN for the finest level. */
static uint32_t level_end_rgn(const tile_t *t, int level)
{
    if (level + 1 < t->n_levels) {
        uint32_t stride;
        uint32_t off = level_rec_off(t, level + 1, &stride);
        uint8_t b[3];
        if (TRE_READ(t, t->tre2_off + off, b, 3)) return rd24(b);
    }
    return (t->rgn_size > t->rgn_data0) ? t->rgn_size - t->rgn_data0 : 0;
}

static void parse_subdiv(const uint8_t *rec, uint8_t bpc, subdiv_t *sd)
{
    uint16_t w = rd16(rec + 10);
    sd->rgn_off = rd24(rec);
    sd->elem    = rec[3];
    sd->clon    = rds24(rec + 4);
    sd->clat    = rds24(rec + 7);
    sd->w       = w & 0x7FFF;
    sd->h       = rd16(rec + 12);
    sd->bpc     = bpc;
}

/* Scan the pseudo-FAT and index every tile. Forward-only through the file. */
static bool build_index(void)
{
    uint8_t *fat = (uint8_t *)GALLOC(FAT_BATCH);
    if (!fat) return false;

    /* Names pair a tile's TRE with its RGN, and they have to be kept for the
     * whole scan rather than a window of recent ones: a gmapsupp is free to
     * list every TRE and then every RGN, which a window would fail to pair for
     * anything but a small map. Held only while indexing, then freed. */
    char    *names = NULL;
    uint32_t n_names = 0, cap_names = 0;

    bool done = false, saw_nt = false;
    uint64_t off = 0x400;

    while (!done) {
        uint32_t want = FAT_BATCH;
        if (off + want > s_src_size) want = (uint32_t)(s_src_size - off);
        if (want < 0x200) break;
        if (!fh_read(off, fat, want)) break;
        STAT(reads, 1);
        STAT(bytes, want);

        for (uint32_t k = 0; k + 0x200 <= want; k += 0x200) {
            const uint8_t *slot = fat + k;
            if (slot[0] != 0x01) { done = true; break; }

            char name[9], ext[4];
            memcpy(name, slot + 1, 8); name[8] = 0;
            memcpy(ext, slot + 9, 3);  ext[3] = 0;
            for (int i = 7; i >= 0 && name[i] == ' '; i--) name[i] = 0;
            for (int i = 2; i >= 0 && ext[i] == ' '; i--) ext[i] = 0;
            if (!name[0]) continue;

            if (strcmp(ext, "NT") == 0) { saw_nt = true; done = true; break; }
            bool is_tre = strcmp(ext, "TRE") == 0;
            bool is_rgn = strcmp(ext, "RGN") == 0;
            if (!is_tre && !is_rgn) continue;

            uint32_t size = rd32(slot + 12);

            /* Find or create the tile. */
            int found = -1;
            for (uint32_t i = 0; i < n_names; i++)
                if (strncmp(names + i * 9, name, 8) == 0) { found = (int)i; break; }
            if (found < 0) {
                s_tiles = (tile_t *)grow(s_tiles, &s_cap_tiles, s_n_tiles + 1, sizeof(tile_t));
                if (!s_tiles) { free(fat); free(names); return false; }
                char *nn = (char *)grow(names, &cap_names, n_names + 1, 9);
                if (!nn) { free(fat); free(names); return false; }
                names = nn;
                found = (int)s_n_tiles;
                memset(&s_tiles[found], 0, sizeof(tile_t));
                s_tiles[found].run_tre = s_tiles[found].run_rgn = 0xFFFFFFFFu;
                s_n_tiles++;
                strncpy(names + (size_t)n_names * 9, name, 8);
                names[(size_t)n_names * 9 + 8] = 0;
                n_names++;
            }
            tile_t *t = &s_tiles[found];

            /* Turn the block list into runs. */
            uint32_t first_run = s_n_runs;
            uint32_t added = 0;
            int32_t  prev = -1;
            for (int i = 0; i < 240; i++) {
                uint16_t b = rd16(slot + 0x20 + i * 2);
                if (b == 0xFFFF) break;
                if (prev >= 0 && (uint32_t)b == (uint32_t)prev + 1) {
                    s_runs[s_n_runs - 1].count++;
                } else {
                    s_runs = (blkrun_t *)grow(s_runs, &s_cap_runs, s_n_runs + 1, sizeof(blkrun_t));
                    if (!s_runs) { free(fat); return false; }
                    s_runs[s_n_runs].start = b;
                    s_runs[s_n_runs].count = 1;
                    s_n_runs++;
                    added++;
                }
                prev = b;
            }

            uint32_t *run_slot   = is_tre ? &t->run_tre    : &t->run_rgn;
            uint16_t *run_count  = is_tre ? &t->n_tre_runs : &t->n_rgn_runs;
            uint32_t *size_slot  = is_tre ? &t->tre_size   : &t->rgn_size;
            if (*run_slot == 0xFFFFFFFFu) *run_slot = first_run;
            *run_count = (uint16_t)(*run_count + added);
            if (rd16(slot + 16) == 0) *size_slot = size;
        }
        off += want;
    }
    free(fat);
    free(names);

    if (saw_nt) {
        GLOG("[IMG] this map is in Garmin NT format, which is not supported\n");
        return false;
    }
    GLOG("[IMG] FAT scan found %u tiles, %u block runs\n",
         (unsigned)s_n_tiles, (unsigned)s_n_runs);

    /* Read every tile header in one ascending sweep over the file.
     *
     * Ordering is not a nicety here. Without fast-seek a backward seek restarts
     * the FAT cluster walk from byte zero, which on a multi-gigabyte map costs
     * most of a second. Sorting by TRE alone was not enough: mkgmap writes RGN
     * BEFORE TRE, so reading a tile's TRE and then its RGN jumped backwards
     * once per tile — 332 tiles took eleven minutes almost entirely in seeks.
     * Sorting the individual subfile reads, rather than the tiles, keeps the
     * file position monotonic and the walk incremental. */
    uint32_t n_work = s_n_tiles * 2;
    uint32_t *work = (uint32_t *)GALLOC(n_work * sizeof(uint32_t));   /* tile<<1 | is_rgn */
    uint32_t *key  = (uint32_t *)GALLOC(n_work * sizeof(uint32_t));
    if (!work || !key) { free(work); free(key); return false; }

    uint32_t nw = 0;
    for (uint32_t i = 0; i < s_n_tiles; i++) {
        tile_t *t = &s_tiles[i];
        t->n_levels = 0;
        if (t->run_tre == 0xFFFFFFFFu || t->run_rgn == 0xFFFFFFFFu ||
            !t->tre_size || !t->rgn_size) continue;
        key[nw]  = s_runs[t->run_tre].start; work[nw++] = (i << 1) | 0u;
        key[nw]  = s_runs[t->run_rgn].start; work[nw++] = (i << 1) | 1u;
    }
    for (uint32_t i = 1; i < nw; i++) {
        uint32_t kv = key[i], wv = work[i];
        uint32_t j = i;
        while (j && key[j - 1] > kv) { key[j] = key[j - 1]; work[j] = work[j - 1]; j--; }
        key[j] = kv; work[j] = wv;
    }

    GLOG("[IMG] indexing %u tiles (first open only)...\n", (unsigned)s_n_tiles);
    uint8_t *tre_ok = (uint8_t *)GALLOC(s_n_tiles);
    uint8_t *rgn_ok = (uint8_t *)GALLOC(s_n_tiles);
    if (!tre_ok || !rgn_ok) { free(work); free(key); free(tre_ok); free(rgn_ok); return false; }
    memset(tre_ok, 0, s_n_tiles);
    memset(rgn_ok, 0, s_n_tiles);

    for (uint32_t k = 0; k < nw; k++) {
        uint32_t ti = work[k] >> 1;
        if (work[k] & 1) rgn_ok[ti] = index_tile_rgn(&s_tiles[ti]) ? 1 : 0;
        else             tre_ok[ti] = index_tile_tre(&s_tiles[ti]) ? 1 : 0;
        if ((k & 0xFF) == 0xFF)
            GLOG("[IMG]   %u/%u headers\n", (unsigned)(k + 1), (unsigned)nw);
    }
    free(work);
    free(key);

    uint32_t ok_count = 0;
    for (uint32_t i = 0; i < s_n_tiles; i++) {
        if (tre_ok[i] && rgn_ok[i]) ok_count++;
        else s_tiles[i].n_levels = 0;     /* marks it for removal below */
    }
    free(tre_ok);
    free(rgn_ok);

    /* Compact out the tiles that did not come together. lvl_first indexes the
     * pooled level array, so moving the structs keeps them valid. */
    uint32_t good = 0;
    for (uint32_t i = 0; i < s_n_tiles; i++) {
        if (!s_tiles[i].n_levels) continue;
        if (good != i) s_tiles[good] = s_tiles[i];
        good++;
    }
    s_n_tiles = good;
    GLOG("[IMG] %u tiles usable of %u\n", (unsigned)good, (unsigned)ok_count);
    return s_n_tiles > 0;
}

/* ---- sidecar ----
 * Rebuilding the index means touching every tile header across the whole file.
 * On a multi-gigabyte map that is seconds, so it is written out beside the map
 * and reused. The source size is the validity check: a different .img at the
 * same path will not match. */
static void sidecar_path(const char *img, char *out, size_t cap)
{
    snprintf(out, cap, "%s.tdx", img);
}

typedef struct {
    char     magic[6];
    uint16_t ver;
    uint64_t src_size;
    uint32_t block_size;
    uint32_t n_tiles, n_runs, n_lvls;
    uint32_t sz_tile, sz_run, sz_lvl;
} sidecar_hdr_t;

static bool sidecar_load(const char *path)
{
#ifdef ARDUINO
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    /* Probe first: opening a missing file for read logs an error, and on the
     * first run for any map the sidecar is missing by definition. */
    if (!SD.exists(path)) { shared_spi_unlock(); return false; }
    File f = SD.open(path, FILE_READ);
    if (!f) { shared_spi_unlock(); return false; }
    sidecar_hdr_t h;
    bool ok = f.read((uint8_t *)&h, sizeof(h)) == (int)sizeof(h);
    if (ok) ok = memcmp(h.magic, SIDECAR_MAGIC, 6) == 0 && h.ver == SIDECAR_VER &&
                 h.src_size == s_src_size && h.sz_tile == sizeof(tile_t) &&
                 h.sz_run == sizeof(blkrun_t) && h.sz_lvl == sizeof(lvl_t) &&
                 h.n_tiles && h.n_runs && h.n_lvls;
    if (ok) {
        s_block_size = h.block_size;
        s_tiles = (tile_t *)GALLOC((size_t)h.n_tiles * sizeof(tile_t));
        s_runs  = (blkrun_t *)GALLOC((size_t)h.n_runs * sizeof(blkrun_t));
        s_lvls  = (lvl_t *)GALLOC((size_t)h.n_lvls * sizeof(lvl_t));
        ok = s_tiles && s_runs && s_lvls &&
             f.read((uint8_t *)s_tiles, h.n_tiles * sizeof(tile_t)) > 0 &&
             f.read((uint8_t *)s_runs,  h.n_runs  * sizeof(blkrun_t)) > 0 &&
             f.read((uint8_t *)s_lvls,  h.n_lvls  * sizeof(lvl_t)) > 0;
        if (ok) {
            s_n_tiles = s_cap_tiles = h.n_tiles;
            s_n_runs  = s_cap_runs  = h.n_runs;
            s_n_lvls  = s_cap_lvls  = h.n_lvls;
        }
    }
    f.close();
    shared_spi_unlock();
    return ok;
#else
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    sidecar_hdr_t h;
    bool ok = fread(&h, 1, sizeof(h), f) == sizeof(h) &&
              memcmp(h.magic, SIDECAR_MAGIC, 6) == 0 && h.ver == SIDECAR_VER &&
              h.src_size == s_src_size && h.sz_tile == sizeof(tile_t) &&
              h.sz_run == sizeof(blkrun_t) && h.sz_lvl == sizeof(lvl_t) &&
              h.n_tiles && h.n_runs && h.n_lvls;
    if (ok) {
        s_block_size = h.block_size;
        s_tiles = (tile_t *)malloc((size_t)h.n_tiles * sizeof(tile_t));
        s_runs  = (blkrun_t *)malloc((size_t)h.n_runs * sizeof(blkrun_t));
        s_lvls  = (lvl_t *)malloc((size_t)h.n_lvls * sizeof(lvl_t));
        ok = s_tiles && s_runs && s_lvls &&
             fread(s_tiles, sizeof(tile_t), h.n_tiles, f) == h.n_tiles &&
             fread(s_runs,  sizeof(blkrun_t), h.n_runs, f) == h.n_runs &&
             fread(s_lvls,  sizeof(lvl_t), h.n_lvls, f) == h.n_lvls;
        if (ok) {
            s_n_tiles = s_cap_tiles = h.n_tiles;
            s_n_runs  = s_cap_runs  = h.n_runs;
            s_n_lvls  = s_cap_lvls  = h.n_lvls;
        }
    }
    fclose(f);
    return ok;
#endif
}

static void sidecar_store(const char *path)
{
    sidecar_hdr_t h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, SIDECAR_MAGIC, 6);
    h.ver = SIDECAR_VER;
    h.src_size = s_src_size;
    h.block_size = s_block_size;
    h.n_tiles = s_n_tiles; h.n_runs = s_n_runs; h.n_lvls = s_n_lvls;
    h.sz_tile = sizeof(tile_t); h.sz_run = sizeof(blkrun_t); h.sz_lvl = sizeof(lvl_t);

    if (sidecar_write(path, &h, sizeof(h),
                      s_tiles, s_n_tiles * (uint32_t)sizeof(tile_t),
                      s_runs,  s_n_runs  * (uint32_t)sizeof(blkrun_t),
                      s_lvls,  s_n_lvls  * (uint32_t)sizeof(lvl_t)))
        GLOG("[IMG] wrote index cache %s\n", path);
    else
        GLOG("[IMG] could not write index cache (map still works, just slower to open)\n");
}

static void compute_bounds(void)
{
    s_min_lat = s_min_lon = 0x7FFFFFFF;
    s_max_lat = s_max_lon = -0x7FFFFFFF;
    for (uint32_t i = 0; i < s_n_tiles; i++) {
        const tile_t *t = &s_tiles[i];
        int32_t s = gu_to_e7(t->south), n = gu_to_e7(t->north);
        int32_t w = gu_to_e7(t->west),  e = gu_to_e7(t->east);
        if (s < s_min_lat) s_min_lat = s;
        if (n > s_max_lat) s_max_lat = n;
        if (w < s_min_lon) s_min_lon = w;
        if (e > s_max_lon) s_max_lon = e;
    }
}

bool gimg_open(const char *path)
{
    gimg_close();
    if (!fh_open(path)) { GLOG("[IMG] cannot open %s\n", path); return false; }
    s_open = true;
    s_src_size = fh_size();

    uint8_t hdr[0x200];
    if (!fh_read(0, hdr, sizeof(hdr))) { GLOG("[IMG] short header\n"); gimg_close(); return false; }
    if (memcmp(hdr + 0x10, "DSKIMG", 6) != 0) {
        GLOG("[IMG] not a Garmin .img (no DSKIMG signature)\n");
        gimg_close(); return false;
    }
    if (hdr[0] != 0x00) {
        GLOG("[IMG] XOR key %02x is not supported\n", hdr[0]);
        gimg_close(); return false;
    }
    s_block_size = 1u << (hdr[0x61] + hdr[0x62]);
    if (s_block_size < 512 || s_block_size > 65536) {
        GLOG("[IMG] odd block size %u\n", (unsigned)s_block_size);
        gimg_close(); return false;
    }

    char scp[160];
    sidecar_path(path, scp, sizeof(scp));
#ifdef ARDUINO
    uint32_t t_start = millis();
#endif
    bool cached = sidecar_load(scp);
    if (cached) {
        GLOG("[IMG] index cache hit\n");
    } else {
        GLOG("[IMG] no index cache; scanning %.1f MB\n", s_src_size / 1048576.0);
        if (!build_index()) { GLOG("[IMG] no usable map tiles\n"); gimg_close(); return false; }
        sidecar_store(scp);
    }
#ifdef ARDUINO
    GLOG("[IMG] index ready in %lu ms\n", (unsigned long)(millis() - t_start));
#endif

    compute_bounds();

    s_chunk   = (uint8_t *)GALLOC(CHUNK_CAP);
    s_sdbuf   = (uint8_t *)GALLOC((SUBDIV_BATCH + 1) * 16);
    s_lat_buf = (int32_t *)GALLOC(GIMG_MAX_POINTS * sizeof(int32_t));
    s_lon_buf = (int32_t *)GALLOC(GIMG_MAX_POINTS * sizeof(int32_t));
    if (!s_chunk || !s_sdbuf || !s_lat_buf || !s_lon_buf) {
        GLOG("[IMG] out of memory\n"); gimg_close(); return false;
    }

    uint32_t n_sub = 0;
    int max_lv = 0;
    for (uint32_t i = 0; i < s_n_tiles; i++) {
        for (int l = 0; l < s_tiles[i].n_levels; l++)
            n_sub += s_lvls[s_tiles[i].lvl_first + l].count;
        if (s_tiles[i].n_levels > max_lv) max_lv = s_tiles[i].n_levels;
    }
    snprintf(s_desc, sizeof(s_desc), "%u tile%s, %u subdiv, %d levels%s",
             (unsigned)s_n_tiles, s_n_tiles == 1 ? "" : "s",
             (unsigned)n_sub, max_lv, cached ? " (cached)" : "");
    GLOG("[IMG] %s: %s, lat %.4f..%.4f lon %.4f..%.4f\n", path, s_desc,
         s_min_lat / 1e7, s_max_lat / 1e7, s_min_lon / 1e7, s_max_lon / 1e7);
    return true;
}

const char *gimg_describe(void) { return s_open ? s_desc : "no map"; }

void gimg_bounds(int32_t *mnla, int32_t *mnlo, int32_t *mxla, int32_t *mxlo)
{
    if (mnla) *mnla = s_min_lat;
    if (mnlo) *mnlo = s_min_lon;
    if (mxla) *mxla = s_max_lat;
    if (mxlo) *mxlo = s_max_lon;
}

int gimg_overlap_tiles(int32_t min_lat, int32_t min_lon, int32_t max_lat, int32_t max_lon)
{
    if (!s_open) return 0;
    int32_t q_s = e7_to_gu(min_lat), q_n = e7_to_gu(max_lat);
    int32_t q_w = e7_to_gu(min_lon), q_e = e7_to_gu(max_lon);
    int n = 0;
    for (uint32_t i = 0; i < s_n_tiles; i++) {
        const tile_t *t = &s_tiles[i];
        if (t->east < q_w || t->west > q_e || t->north < q_s || t->south > q_n) continue;
        n++;
    }
    return n;
}

int gimg_level_count(void)
{
    int m = 0;
    for (uint32_t i = 0; i < s_n_tiles; i++)
        if (s_tiles[i].n_levels > m) m = s_tiles[i].n_levels;
    return m;
}

/* ------------------------------------------------------------------ query */
/* A subdivision selected for drawing, with the absolute file offset used to
 * put the reads in forward order. */
typedef struct { subdiv_t sd; uint32_t tile; uint64_t pos; } hit_t;

/* Walk a tile's subdivisions at one level, in batches, calling `visit`. */
static int scan_level(uint32_t ti, int level,
                      int32_t q_s, int32_t q_w, int32_t q_n, int32_t q_e,
                      hit_t *hits, int max_hits, int n_hits)
{
    const tile_t *t = &s_tiles[ti];
    if (level < 0 || level >= t->n_levels) return n_hits;
    const lvl_t *L = &s_lvls[t->lvl_first + level];
    if (!L->count) return n_hits;

    uint32_t stride;
    uint32_t rec_off = level_rec_off(t, level, &stride);
    uint32_t tail_end = level_end_rgn(t, level);

    for (uint32_t done = 0; done < L->count; ) {
        uint32_t batch = L->count - done;
        if (batch > SUBDIV_BATCH) batch = SUBDIV_BATCH;
        /* One record of lookahead so each subdivision knows where the next
         * one's data begins; the extra costs nothing, being contiguous. */
        uint32_t fetch = (done + batch < L->count) ? batch + 1 : batch;
        if (!TRE_READ(t, t->tre2_off + rec_off + done * stride, s_sdbuf, fetch * stride)) break;

        for (uint32_t j = 0; j < batch; j++) {
            subdiv_t sd;
            parse_subdiv(s_sdbuf + j * stride, L->bpc, &sd);
            sd.rgn_end = (j + 1 < fetch) ? rd24(s_sdbuf + (j + 1) * stride) : tail_end;
            /* A subdivision with no object classes holds nothing to draw. The
             * coarse levels of a contour map are entirely these, and counting
             * them would let pick_level settle on an empty level. */
            if (!sd.elem) continue;
            int sh = 24 - sd.bpc;
            int32_t hw = (int32_t)sd.w << sh;
            int32_t hh = (int32_t)sd.h << sh;
            if (sd.clat + hh < q_s || sd.clat - hh > q_n) continue;
            if (sd.clon + hw < q_w || sd.clon - hw > q_e) continue;
            if (n_hits >= max_hits) return n_hits;

            uint32_t contig;
            hits[n_hits].sd   = sd;
            hits[n_hits].tile = ti;
            hits[n_hits].pos  = sub_locate(s_runs + t->run_rgn, t->n_rgn_runs,
                                           t->rgn_data0 + sd.rgn_off, &contig);
            n_hits++;
        }
        done += batch;
    }
    return n_hits;
}

int gimg_pick_level(int32_t min_lat, int32_t min_lon, int32_t max_lat, int32_t max_lon)
{
    if (!s_open || !s_n_tiles) return 0;
    if (min_lat > max_lat) { int32_t t = min_lat; min_lat = max_lat; max_lat = t; }
    if (min_lon > max_lon) { int32_t t = min_lon; min_lon = max_lon; max_lon = t; }

    int32_t q_s = e7_to_gu(min_lat), q_n = e7_to_gu(max_lat);
    int32_t q_w = e7_to_gu(min_lon), q_e = e7_to_gu(max_lon);

    /* Choose by how much work the level implies, not by how big its tiles are.
     * Garmin levels differ in generalisation rather than extent — at a map's
     * coarse levels one subdivision can span a whole island group — so sizing
     * off the subdivision box picks a level far too detailed. The count of
     * overlapping subdivisions tracks the amount of geometry and is what
     * decides whether a redraw finishes before the panel does. */
    const int budget = 12;
    int best = 0;
    int n_lv = gimg_level_count();
    hit_t *scratch = (hit_t *)GALLOC(sizeof(hit_t) * (size_t)(budget + 1));
    if (!scratch) return 0;

    for (int lv = 0; lv < n_lv; lv++) {
        int n = 0, tiles = 0;
        for (uint32_t ti = 0; ti < s_n_tiles && n <= budget; ti++) {
            const tile_t *t = &s_tiles[ti];
            if (t->east < q_w || t->west > q_e || t->north < q_s || t->south > q_n) continue;
            if (++tiles > MAX_TILES_PER_Q) { n = budget + 1; break; }   /* too wide */
            n = scan_level(ti, lv, q_s, q_w, q_n, q_e, scratch, budget + 1, n);
        }
        if (n == 0) continue;              /* nothing here; not a useful choice */
        if (n <= budget) best = lv;
        /* Levels run coarse to fine and subdivision counts only grow, so once
         * one is over budget every finer one is too. Without this the scan
         * visited all nine levels of every overlapping tile, which on a
         * country-sized map is thousands of seeks in a single redraw. */
        else break;
    }
    free(scratch);
    return best;
}

int gimg_query(int level,
               int32_t min_lat, int32_t min_lon, int32_t max_lat, int32_t max_lon,
               gimg_feature_cb cb, void *user)
{
    if (!s_open) return -1;
    if (min_lat > max_lat) { int32_t t = min_lat; min_lat = max_lat; max_lat = t; }
    if (min_lon > max_lon) { int32_t t = min_lon; min_lon = max_lon; max_lon = t; }

    int32_t q_s = e7_to_gu(min_lat), q_n = e7_to_gu(max_lat);
    int32_t q_w = e7_to_gu(min_lon), q_e = e7_to_gu(max_lon);

    hit_t *hits = (hit_t *)GALLOC(sizeof(hit_t) * MAX_HITS);
    if (!hits) return -1;
    int n_hits = 0;

    int tiles = 0;
    for (uint32_t ti = 0; ti < s_n_tiles; ti++) {
        const tile_t *t = &s_tiles[ti];
        if (t->east < q_w || t->west > q_e || t->north < q_s || t->south > q_n) continue;
        if (++tiles > MAX_TILES_PER_Q) {
            GLOG("[IMG] viewport spans more than %d tiles; drawing the first %d\n",
                 MAX_TILES_PER_Q, MAX_TILES_PER_Q);
            break;
        }
        n_hits = scan_level(ti, level, q_s, q_w, q_n, q_e, hits, MAX_HITS, n_hits);
    }
    if (n_hits == MAX_HITS)
        GLOG("[IMG] viewport hit the %d-subdivision cap; drawing a subset\n", MAX_HITS);

    /* Sort by file position. Without fast-seek a backward seek restarts the
     * cluster walk from the start of the file, so reading in ascending order
     * is the difference between a redraw and a stall. Insertion sort: the list
     * is short and usually near-sorted already. */
    for (int i = 1; i < n_hits; i++) {
        hit_t v = hits[i];
        int j = i;
        while (j > 0 && hits[j - 1].pos > v.pos) { hits[j] = hits[j - 1]; j--; }
        hits[j] = v;
    }

    int total = 0;
    for (int i = 0; i < n_hits; i++) {
        STAT(subdivs, 1);
        total += decode_lines(&s_tiles[hits[i].tile], &hits[i].sd, GIMG_POLYGON,  cb, user);
        total += decode_lines(&s_tiles[hits[i].tile], &hits[i].sd, GIMG_POLYLINE, cb, user);
    }
    free(hits);
    return total;
}
