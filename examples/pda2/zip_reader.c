/**
 * @file      zip_reader.c
 * @brief     Minimal read-only ZIP reader. See zip_reader.h.
 */
#include "zip_reader.h"
#include "inflate_util.h"
#include <stdlib.h>
#include <string.h>

#define SIG_EOCD   0x06054b50u
#define SIG_CD     0x02014b50u
#define SIG_LOCAL  0x04034b50u

/* Central-directory record field offsets. */
#define CD_METHOD    10
#define CD_CSIZE     20
#define CD_USIZE     24
#define CD_NAMELEN   28
#define CD_EXTRALEN  30
#define CD_CMTLEN    32
#define CD_LOCALOFF  42
#define CD_FIXED     46

/* Local-header field offsets. */
#define LH_NAMELEN   26
#define LH_EXTRALEN  28
#define LH_FIXED     30

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int zip_open(zip_t *z, const uint8_t *data, size_t len)
{
    if (!data || len < 22) return ZIP_ERR_FORMAT;

    /* The end-of-central-directory record sits at the very end, followed only
     * by an optional comment (<= 64 KB), so scan backwards for its signature. */
    size_t max_back = len < (22 + 65535u) ? len : (22 + 65535u);
    const uint8_t *eocd = NULL;
    for (size_t back = 22; back <= max_back; back++) {
        const uint8_t *p = data + len - back;
        if (rd32(p) == SIG_EOCD) { eocd = p; break; }
    }
    if (!eocd) return ZIP_ERR_FORMAT;

    uint16_t count   = rd16(eocd + 10);
    uint32_t cd_size = rd32(eocd + 12);
    uint32_t cd_off  = rd32(eocd + 16);

    /* Any of these saturated means the real values live in a Zip64 record. */
    if (count == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu)
        return ZIP_ERR_ZIP64;
    if ((size_t)cd_off + cd_size > len) return ZIP_ERR_FORMAT;

    z->data   = data;
    z->len    = len;
    z->cd_off = cd_off;
    z->count  = count;
    return 0;
}

/* Walk to the central-directory record for `idx`, returning NULL if the
 * directory is malformed or idx is out of range. */
static const uint8_t *cd_record(const zip_t *z, int idx)
{
    if (idx < 0 || idx >= (int)z->count) return NULL;

    const uint8_t *p = z->data + z->cd_off;
    const uint8_t *end = z->data + z->len;

    for (int i = 0; i < idx; i++) {
        if (p + CD_FIXED > end || rd32(p) != SIG_CD) return NULL;
        size_t step = CD_FIXED + rd16(p + CD_NAMELEN) +
                      rd16(p + CD_EXTRALEN) + rd16(p + CD_CMTLEN);
        if (p + step > end) return NULL;
        p += step;
    }
    if (p + CD_FIXED > end || rd32(p) != SIG_CD) return NULL;
    if (p + CD_FIXED + rd16(p + CD_NAMELEN) > end) return NULL;
    return p;
}

int zip_find(const zip_t *z, const char *name)
{
    size_t want = strlen(name);
    for (int i = 0; i < (int)z->count; i++) {
        const uint8_t *cd = cd_record(z, i);
        if (!cd) return -1;
        uint16_t nlen = rd16(cd + CD_NAMELEN);
        if (nlen == want && memcmp(cd + CD_FIXED, name, want) == 0) return i;
    }
    return -1;
}

bool zip_entry_name(const zip_t *z, int idx, char *out, size_t cap)
{
    const uint8_t *cd = cd_record(z, idx);
    if (!cd || cap == 0) return false;
    uint16_t nlen = rd16(cd + CD_NAMELEN);
    if (nlen > cap - 1) nlen = (uint16_t)(cap - 1);
    memcpy(out, cd + CD_FIXED, nlen);
    out[nlen] = '\0';
    return true;
}

uint32_t zip_entry_size(const zip_t *z, int idx)
{
    const uint8_t *cd = cd_record(z, idx);
    return cd ? rd32(cd + CD_USIZE) : 0;
}

int zip_extract(const zip_t *z, int idx, uint8_t **out, size_t *out_len)
{
    const uint8_t *cd = cd_record(z, idx);
    if (!cd) return ZIP_ERR_FORMAT;

    uint16_t method = rd16(cd + CD_METHOD);
    uint32_t csize  = rd32(cd + CD_CSIZE);
    uint32_t usize  = rd32(cd + CD_USIZE);
    uint32_t loff   = rd32(cd + CD_LOCALOFF);

    if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu) return ZIP_ERR_ZIP64;
    if ((size_t)loff + LH_FIXED > z->len) return ZIP_ERR_FORMAT;

    const uint8_t *lh = z->data + loff;
    if (rd32(lh) != SIG_LOCAL) return ZIP_ERR_FORMAT;

    /* The local header repeats the name and carries its own extra field, whose
     * length often differs from the central directory's — always use this one. */
    size_t data_off = (size_t)loff + LH_FIXED + rd16(lh + LH_NAMELEN) + rd16(lh + LH_EXTRALEN);
    if (data_off + csize > z->len) return ZIP_ERR_FORMAT;
    const uint8_t *src = z->data + data_off;

    if (method == 0) {                       /* stored */
        if (csize != usize) return ZIP_ERR_FORMAT;
        uint8_t *buf = (uint8_t *)malloc((size_t)usize + 1);
        if (!buf) return ZIP_ERR_MEMORY;
        memcpy(buf, src, usize);
        buf[usize] = '\0';
        *out = buf;
        *out_len = usize;
        return 0;
    }

    if (method != 8) return ZIP_ERR_METHOD;

    uint8_t *raw = NULL;
    size_t rawlen = 0;
    if (inflate_raw(src, csize, &raw, &rawlen) != 0) {
        free(raw);
        return ZIP_ERR_DATA;
    }
    if (usize != 0 && rawlen != usize) {     /* header disagrees with the data */
        free(raw);
        return ZIP_ERR_DATA;
    }

    /* Append the NUL the callers rely on; lodepng sized the buffer exactly. */
    uint8_t *buf = (uint8_t *)realloc(raw, rawlen + 1);
    if (!buf) { free(raw); return ZIP_ERR_MEMORY; }
    buf[rawlen] = '\0';

    *out = buf;
    *out_len = rawlen;
    return 0;
}
