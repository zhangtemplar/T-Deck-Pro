/**
 * @file      epub.h
 * @brief     EPUB container parsing: find the chapters, in reading order.
 *
 * An EPUB is a ZIP holding XHTML documents plus a package file (.opf) that
 * lists them (the manifest) and gives their reading order (the spine). This
 * resolves that chain — container.xml -> .opf -> spine -> chapter paths — with
 * lenient attribute scanning rather than a real XML parser, which is enough for
 * the handful of attributes involved and far smaller.
 *
 * The caller owns the ZIP buffer and must keep it alive for the epub_t's life.
 */
#ifndef __EPUB_H__
#define __EPUB_H__

#include "zip_reader.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EPUB_MAX_CHAPTERS 400
#define EPUB_PATH_LEN     160

#define EPUB_TITLE_LEN 64

typedef struct {
    zip_t zip;
    char  title[96];
    char  opf_dir[EPUB_PATH_LEN];              /* directory holding the .opf   */
    int   nchap;
    char (*chap)[EPUB_PATH_LEN];               /* zip paths, in spine order    */
    char (*chap_title)[EPUB_TITLE_LEN];        /* from the TOC; "" if unknown  */
} epub_t;

#define EPUB_ERR_FORMAT (-1)   /* not an epub / no spine we can use */
#define EPUB_ERR_DRM    (-2)   /* encrypted (Adobe ADEPT and friends) */
#define EPUB_ERR_MEMORY (-3)
#define EPUB_ERR_ZIP    (-4)   /* unreadable archive */

/* Parse the container. Returns 0 on success; see EPUB_ERR_*. */
int  epub_open(epub_t *e, const uint8_t *data, size_t len);

/* Release the chapter table (not the caller's ZIP buffer). */
void epub_close(epub_t *e);

/* Extract chapter `idx` as NUL-terminated XHTML. Caller frees *out. */
int  epub_chapter(const epub_t *e, int idx, uint8_t **out, size_t *len);

/* Human-readable reason for an epub_open failure. */
const char *epub_err_text(int err);

#ifdef __cplusplus
}
#endif

#endif /* __EPUB_H__ */
