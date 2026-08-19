/**
 * @file      zip_reader.h
 * @brief     Minimal read-only ZIP reader, enough for EPUB containers.
 *
 * Works over a ZIP already held in memory: EPUBs on this device are a couple of
 * megabytes at most, and reading the archive once keeps the SD lock held only
 * for the read rather than across every chapter access.
 *
 * Supports stored (method 0) and deflate (method 8) entries, which is all the
 * EPUB specification permits. Zip64 archives are rejected rather than
 * mis-parsed.
 */
#ifndef __ZIP_READER_H__
#define __ZIP_READER_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         cd_off;     /* offset of the first central-directory record */
    uint16_t       count;      /* number of entries                            */
} zip_t;

#define ZIP_ERR_FORMAT  (-1)   /* not a zip / truncated       */
#define ZIP_ERR_ZIP64   (-2)   /* zip64 archive, unsupported  */
#define ZIP_ERR_METHOD  (-3)   /* compression method we can't handle */
#define ZIP_ERR_MEMORY  (-4)
#define ZIP_ERR_DATA    (-5)   /* decompression failed / size mismatch */

/* Parse the central directory. Returns 0 on success. */
int zip_open(zip_t *z, const uint8_t *data, size_t len);

/* Index of the entry with this exact name, or -1. Case-sensitive, as ZIP is. */
int zip_find(const zip_t *z, const char *name);

/* Copy entry `idx`'s name into `out`. Returns false if idx is out of range. */
bool zip_entry_name(const zip_t *z, int idx, char *out, size_t cap);

/* Uncompressed size of entry `idx`, or 0 if out of range. */
uint32_t zip_entry_size(const zip_t *z, int idx);

/* Extract entry `idx`. *out is malloc'd with one extra NUL byte so the result
 * can be treated as a C string; *out_len excludes that NUL. Caller frees. */
int zip_extract(const zip_t *z, int idx, uint8_t **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* __ZIP_READER_H__ */
