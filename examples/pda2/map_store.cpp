/**
 * @file      map_store.cpp
 * @brief     Map discovery and zip unpacking. See map_store.h.
 */
#include "map_store.h"

#include <Arduino.h>
#include <SD.h>
#include <Preferences.h>
#include <string.h>
#include <strings.h>

#include "utilities.h"
#include "inflate_util.h"

extern void shared_spi_lock(void);
extern void shared_spi_unlock(void);
extern void shared_spi_prepare_device(int cs_pin);
extern bool sd_ensure_mounted(void);

/* One-shot inflate needs the compressed input and the whole output resident at
 * once. Past this the honest answer is to unzip on a computer and copy the
 * .img across, which the picker says in as many words. */
#define UNPACK_INFLATE_MAX (6u * 1024u * 1024u)
#define COPY_CHUNK         (16u * 1024u)

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

static bool ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls > lx && strcasecmp(s + ls - lx, suffix) == 0;
}

/* Strip any directory part; Arduino's File::name() has returned both over the
 * years and neither is worth depending on. */
static const char *base_name(const char *p)
{
    const char *slash = strrchr(p, '/');
    return slash ? slash + 1 : p;
}

int mapstore_list(mapstore_entry_t *out, int max)
{
    int n = 0;
    if (!sd_ensure_mounted()) return -1;      /* distinct from "found nothing" */
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);

    File dir = SD.open(MAPSTORE_DIR);
    if (dir && dir.isDirectory()) {
        for (File f = dir.openNextFile(); f && n < max; f = dir.openNextFile()) {
            if (f.isDirectory()) { f.close(); continue; }
            const char *nm = base_name(f.name());
            bool zip = ends_with_ci(nm, ".zip");
            bool img = ends_with_ci(nm, ".img");
            if ((zip || img) && strlen(nm) < MAPSTORE_NAME_MAX) {
                snprintf(out[n].name, MAPSTORE_NAME_MAX, "%s", nm);
                out[n].is_zip   = zip;
                out[n].size     = (uint32_t)f.size();
                out[n].unpacked = !zip;
                n++;
            }
            f.close();
        }
    }
    if (dir) dir.close();

    /* Mark which zips already have their .img beside them. */
    for (int i = 0; i < n; i++) {
        if (!out[i].is_zip) continue;
        char p[128];
        snprintf(p, sizeof(p), MAPSTORE_DIR "/%.*s.img",
                 (int)(strlen(out[i].name) - 4), out[i].name);
        out[i].unpacked = SD.exists(p);
    }
    shared_spi_unlock();
    return n;
}

/* ---- zip, read straight off the card ---- */

typedef struct {
    uint16_t method;
    uint32_t comp_size, uncomp_size;
    uint32_t data_off;        /* absolute offset of the entry's bytes */
} zentry_t;

/* Locate the .img inside `zf`. The lock must already be held. */
static bool zip_find_img(File &zf, zentry_t *e, char *err, int err_cap)
{
    uint32_t fsize = (uint32_t)zf.size();
    if (fsize < 22) { snprintf(err, err_cap, "zip is truncated"); return false; }

    /* End-of-central-directory: last 22 bytes, plus up to 64 KB of comment. */
    uint32_t tail = fsize < 66000u ? fsize : 66000u;
    uint8_t *buf = (uint8_t *)ps_malloc(tail);
    if (!buf) { snprintf(err, err_cap, "out of memory"); return false; }
    if (!zf.seek(fsize - tail) || zf.read(buf, tail) != (int)tail) {
        free(buf); snprintf(err, err_cap, "cannot read zip tail"); return false;
    }

    int32_t eocd = -1;
    for (int32_t i = (int32_t)tail - 22; i >= 0; i--) {
        if (rd32(buf + i) == 0x06054b50u) { eocd = i; break; }
    }
    if (eocd < 0) { free(buf); snprintf(err, err_cap, "not a zip archive"); return false; }

    uint16_t count  = rd16(buf + eocd + 10);
    uint32_t cd_sz  = rd32(buf + eocd + 12);
    uint32_t cd_off = rd32(buf + eocd + 16);
    free(buf);

    if (cd_off == 0xFFFFFFFFu || count == 0xFFFFu) {
        snprintf(err, err_cap, "zip64 archives are not supported");
        return false;
    }
    if (cd_sz == 0 || cd_sz > 1024u * 1024u) {
        snprintf(err, err_cap, "central directory looks wrong");
        return false;
    }

    uint8_t *cd = (uint8_t *)ps_malloc(cd_sz);
    if (!cd) { snprintf(err, err_cap, "out of memory"); return false; }
    if (!zf.seek(cd_off) || zf.read(cd, cd_sz) != (int)cd_sz) {
        free(cd); snprintf(err, err_cap, "cannot read central directory"); return false;
    }

    bool found = false;
    uint32_t p = 0;
    for (uint16_t i = 0; i < count && p + 46 <= cd_sz; i++) {
        if (rd32(cd + p) != 0x02014b50u) break;
        uint16_t nlen = rd16(cd + p + 28);
        uint16_t elen = rd16(cd + p + 30);
        uint16_t clen = rd16(cd + p + 32);
        if (p + 46 + nlen > cd_sz) break;

        char nm[128];
        uint16_t cn = nlen < sizeof(nm) - 1 ? nlen : (uint16_t)(sizeof(nm) - 1);
        memcpy(nm, cd + p + 46, cn);
        nm[cn] = 0;

        if (ends_with_ci(nm, ".img")) {
            e->method      = rd16(cd + p + 10);
            e->comp_size   = rd32(cd + p + 20);
            e->uncomp_size = rd32(cd + p + 24);
            uint32_t lho   = rd32(cd + p + 42);

            /* The local header repeats the name and extra fields at its own
             * lengths, which need not match the directory's — so the data
             * offset has to come from there, not from arithmetic on this. */
            uint8_t lh[30];
            if (zf.seek(lho) && zf.read(lh, 30) == 30 && rd32(lh) == 0x04034b50u) {
                e->data_off = lho + 30 + rd16(lh + 26) + rd16(lh + 28);
                found = true;
            }
            break;
        }
        p += 46u + nlen + elen + clen;
    }
    free(cd);

    if (!found) snprintf(err, err_cap, "no .img inside the zip");
    return found;
}

static bool unpack(const char *zip_path, const char *img_path,
                   char *err, int err_cap, mapstore_progress_cb cb)
{
    bool ok = false;
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);

    File zf = SD.open(zip_path, FILE_READ);
    if (!zf) { snprintf(err, err_cap, "cannot open %s", zip_path); shared_spi_unlock(); return false; }

    zentry_t e;
    if (!zip_find_img(zf, &e, err, err_cap)) { zf.close(); shared_spi_unlock(); return false; }

    Serial.printf("[MAPSTORE] %s: method %u, %u -> %u bytes\n",
                  zip_path, e.method, (unsigned)e.comp_size, (unsigned)e.uncomp_size);

    File of = SD.open(img_path, FILE_WRITE);
    if (!of) { snprintf(err, err_cap, "cannot create %s", img_path); zf.close(); shared_spi_unlock(); return false; }

    if (e.method == 0) {
        /* Stored: the .img is already sitting there uncompressed, so this is a
         * copy and size is no object. */
        uint8_t *chunk = (uint8_t *)ps_malloc(COPY_CHUNK);
        if (chunk && zf.seek(e.data_off)) {
            uint32_t left = e.uncomp_size;
            ok = true;
            while (left) {
                uint32_t n = left < COPY_CHUNK ? left : COPY_CHUNK;
                if (zf.read(chunk, n) != (int)n || of.write(chunk, n) != n) { ok = false; break; }
                left -= n;
                if (cb) cb("Copying", e.uncomp_size - left, e.uncomp_size);
            }
        } else {
            snprintf(err, err_cap, "out of memory");
        }
        free(chunk);
        if (!ok && !err[0]) snprintf(err, err_cap, "copy failed");

    } else if (e.method == 8) {
        if (e.uncomp_size > UNPACK_INFLATE_MAX) {
            /* Deflate has no random access and the decompressor here is
             * one-shot, so a big map cannot be unpacked on the device. */
            snprintf(err, err_cap, "%luMB is too big to unzip here;\nunzip it and copy the .img",
                     (unsigned long)(e.uncomp_size / (1024 * 1024)));
        } else {
            if (cb) cb("Reading", 0, e.comp_size);
            uint8_t *in = (uint8_t *)ps_malloc(e.comp_size);
            if (!in) {
                snprintf(err, err_cap, "out of memory reading zip");
            } else if (!zf.seek(e.data_off) || zf.read(in, e.comp_size) != (int)e.comp_size) {
                snprintf(err, err_cap, "cannot read zip contents");
                free(in);
                in = NULL;
            }
            if (in) {
                if (cb) cb("Unzipping", 0, 0);
                uint8_t *out = NULL;
                size_t out_len = 0;
                int r = inflate_raw(in, e.comp_size, &out, &out_len);
                free(in);
                if (r != 0 || !out) {
                    snprintf(err, err_cap, "decompression failed");
                } else if (out_len != e.uncomp_size) {
                    snprintf(err, err_cap, "size mismatch after unzip");
                    free(out);
                } else {
                    if (cb) cb("Writing", 0, (uint32_t)out_len);
                    ok = true;
                    size_t left = out_len, done = 0;
                    while (left) {
                        size_t n = left < COPY_CHUNK ? left : COPY_CHUNK;
                        if (of.write(out + done, n) != n) { ok = false; break; }
                        done += n; left -= n;
                        if (cb) cb("Writing", (uint32_t)done, (uint32_t)out_len);
                    }
                    free(out);
                    if (!ok) snprintf(err, err_cap, "write failed (card full?)");
                }
            }
        }
    } else {
        snprintf(err, err_cap, "compression method %u unsupported", e.method);
    }

    of.close();
    zf.close();
    if (!ok) SD.remove(img_path);       /* don't leave a half file to be opened */
    shared_spi_unlock();
    return ok;
}

bool mapstore_resolve(const char *name, char *img_path, int cap,
                      char *err, int err_cap, mapstore_progress_cb cb)
{
    if (err && err_cap) err[0] = 0;
    if (!name || !name[0]) { snprintf(err, err_cap, "no map selected"); return false; }
    if (!sd_ensure_mounted()) { snprintf(err, err_cap, "SD card not mounted"); return false; }

    if (ends_with_ci(name, ".img")) {
        snprintf(img_path, cap, MAPSTORE_DIR "/%s", name);
        return true;
    }
    if (!ends_with_ci(name, ".zip")) { snprintf(err, err_cap, "not a map file"); return false; }

    char zip_path[128];
    snprintf(zip_path, sizeof(zip_path), MAPSTORE_DIR "/%s", name);
    snprintf(img_path, cap, MAPSTORE_DIR "/%.*s.img", (int)(strlen(name) - 4), name);

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    bool have = SD.exists(img_path);
    shared_spi_unlock();
    if (have) return true;              /* unpacked on an earlier run */

    Serial.printf("[MAPSTORE] unpacking %s -> %s\n", zip_path, img_path);
    return unpack(zip_path, img_path, err, err_cap, cb);
}

bool mapstore_get_contours(void)
{
    Preferences p;
    p.begin("gpsmap", false);
    bool v = p.getBool("contour", false);     /* off until asked for */
    p.end();
    return v;
}

void mapstore_set_contours(bool on)
{
    Preferences p;
    p.begin("gpsmap", false);
    p.putBool("contour", on);
    p.end();
}

void mapstore_get_default(char *out, int cap)
{
    /* Opened read-write even to read: a read-only open of a namespace that
     * does not exist yet fails and logs an NVS error on every fresh device. */
    Preferences p;
    p.begin("gpsmap", false);
    String s = p.getString("map", "");
    p.end();
    snprintf(out, cap, "%s", s.c_str());
}

void mapstore_set_default(const char *name)
{
    Preferences p;
    p.begin("gpsmap", false);
    p.putString("map", name ? name : "");
    p.end();
}
