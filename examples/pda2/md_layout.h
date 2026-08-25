/**
 * @file      md_layout.h
 * @brief     Page layout for rendered Markdown: turns blocks into positioned rows.
 *
 * Kept free of LVGL (fonts are opaque handles, metrics come in through
 * callbacks) so the pagination walk — the part with the fiddly resume-mid-block
 * bookkeeping — can be exercised on the host with the same code that runs on
 * the device.
 */
#ifndef __MD_LAYOUT_H__
#define __MD_LAYOUT_H__

#include <stdint.h>
#include "md_parse.h"
#include "text_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where rendering is up to: a block, and the byte offset within that block's
 * stripped text where the next page begins.
 *
 * A byte offset rather than a line index because resuming has to be O(1).
 * Indexing by line meant md_layout_page re-wrapped the block from its start
 * and threw away every line before the cursor — fine for a paragraph, but a
 * chapter that is one long block turned paging into quadratic work, and
 * stepping back across a chapter boundary walks every page of it. */
typedef struct {
    int      blk;
    uint32_t off;
} md_cursor_t;

typedef struct {
    /* Glyph advance for a codepoint in the given font. */
    int   (*advance)(uint32_t cp, void *font);
    /* Opaque font handle for a block kind. */
    void *(*font_for)(md_kind_t kind, void *ctx);
    /* Line height of a font, in px (excluding line_gap). */
    int   (*line_height)(void *font);
    /* Left indent for a block, in px. */
    int   (*indent_for)(const md_block_t *b, void *ctx);

    void *ctx;          /* passed to font_for / indent_for */
    int   view_w;       /* usable width, px                */
    int   view_h;       /* usable height, px               */
    int   line_gap;     /* extra px between rows           */
    int   max_rows;     /* hard cap on rows per page       */

    char  *scratch;     /* work buffer for inline-stripped text */
    size_t scratch_sz;
} md_layout_env_t;

/* One laid-out row. `text`/`len` point into the env's scratch buffer and are
 * only valid until the next row is emitted. `line_idx` is the row's index
 * within its block, so 0 means "put the bullet/number marker here". */
typedef void (*md_emit_cb)(void *user, const md_block_t *b, int line_idx,
                           const char *text, size_t len,
                           int x, int y, void *font);

/* Lay out one page starting at `cur`. Pass emit = NULL to measure only.
 * Returns the cursor at which the next page begins; when that equals nblocks
 * the document is finished. Always makes progress provided a single row fits
 * in view_h. */
md_cursor_t md_layout_page(const char *text,
                           const md_block_t *blocks, int nblocks,
                           md_cursor_t cur, const md_layout_env_t *env,
                           md_emit_cb emit, void *user);

#ifdef __cplusplus
}
#endif

#endif /* __MD_LAYOUT_H__ */
