/**
 * @file      map_store.h
 * @brief     Find, unpack and remember the map to draw.
 *
 * Maps arrive from garmin.opentopomap.org as a .zip holding one .img, so /maps
 * may contain either. A .img is opened where it lies. A .zip has its .img
 * extracted alongside it once and the extracted file is used from then on —
 * the map reader needs random access across the whole file, and a DEFLATE
 * stream cannot provide that at any price.
 *
 * The zip is parsed straight off the card rather than through zip_reader,
 * which works on an in-memory archive because EPUBs are small. A map is not.
 * Only the end-of-central-directory record and the central directory itself
 * are read, which is a few hundred bytes however big the archive is.
 */
#ifndef __MAP_STORE_H__
#define __MAP_STORE_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAPSTORE_DIR      "/maps"
#define MAPSTORE_MAX      24
#define MAPSTORE_NAME_MAX 48

typedef struct {
    char name[MAPSTORE_NAME_MAX];   /* file name inside /maps      */
    bool is_zip;
    bool unpacked;                  /* a matching .img already exists */
    uint32_t size;
} mapstore_entry_t;

/* List the .img and .zip files in /maps. Returns how many were written. */
int mapstore_list(mapstore_entry_t *out, int max);

/* Progress during a long extraction, so the screen is not simply frozen.
 * `done`/`total` are bytes; `total` is 0 while it is still unknown. */
typedef void (*mapstore_progress_cb)(const char *what, uint32_t done, uint32_t total);

/* Produce a usable .img path for `name`, unpacking it if that is a zip and it
 * has not been unpacked already. Returns false and fills `err` on failure. */
bool mapstore_resolve(const char *name, char *img_path, int cap,
                      char *err, int err_cap, mapstore_progress_cb cb);

/* Whether contour lines are drawn. Persisted alongside the map choice. */
bool mapstore_get_contours(void);
void mapstore_set_contours(bool on);

/* The map chosen last time, or "" if none. Persisted across reboots. */
void mapstore_get_default(char *out, int cap);
void mapstore_set_default(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* __MAP_STORE_H__ */
