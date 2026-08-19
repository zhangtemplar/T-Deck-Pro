/**
 * @file      md_parse.h
 * @brief     Line-oriented Markdown block parser for the note app's preview.
 *
 * Deliberately block-level only: headings, lists, quotes, fenced code, rules
 * and paragraphs each get their own font and indent. Inline spans (**bold**,
 * *italic*) are not styled — LVGL's Montserrat ships in a single weight, so
 * there is no proportional bold to render them with — the markers are stripped
 * instead so the prose reads cleanly.
 *
 * Blocks reference the source buffer by offset; nothing is copied.
 */
#ifndef __MD_PARSE_H__
#define __MD_PARSE_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MD_PARA = 0,
    MD_H1,
    MD_H2,
    MD_H3,
    MD_BULLET,
    MD_NUMBER,
    MD_QUOTE,
    MD_CODE,      /* one line inside a ``` fence, kept verbatim */
    MD_RULE,      /* --- *** ___                                */
    MD_BLANK,
} md_kind_t;

typedef struct {
    md_kind_t kind;
    size_t    off;     /* offset of the block's text, marker already skipped */
    uint16_t  len;     /* byte length of that text                          */
    uint8_t   indent;  /* list nesting level (0, 1, 2 ...)                  */
    uint16_t  ord;     /* ordered-list number, else 0                       */
} md_block_t;

/* Parse the whole buffer into blocks (one per source line). Fills up to
 * `max_blocks` and returns how many were produced. */
int md_parse(const char *text, size_t len, md_block_t *out, int max_blocks);

/* Copy `src` into `dst`, dropping inline emphasis/code markers and reducing
 * [label](url) to label. Returns the resulting length (always <= src_len).
 * `dst` must have room for src_len bytes. */
size_t md_strip_inline(const char *src, size_t src_len, char *dst);

#ifdef __cplusplus
}
#endif

#endif /* __MD_PARSE_H__ */
