/**
 * @file      text_layout.h
 * @brief     UTF-8 word wrapping and pagination for the text reader.
 *
 * LVGL's own line breaker only breaks on " ,.;:-_" and has long-word breaking
 * disabled, so a Chinese paragraph — which contains no spaces — would be one
 * unbreakable "word" and would not wrap at all. This does the wrapping instead:
 * ASCII word wrap plus CJK breaking (which may occur between almost any two
 * ideographs, subject to the usual kinsoku rules about punctuation that may not
 * start or end a line).
 *
 * Glyph widths come in through a callback so this stays free of LVGL and can be
 * exercised on the host.
 */
#ifndef __TEXT_LAYOUT_H__
#define __TEXT_LAYOUT_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Width in pixels the given codepoint advances the cursor. */
typedef int (*txt_advance_cb)(uint32_t cp, void *ctx);

typedef struct {
    txt_advance_cb advance;
    void *ctx;
    int width;         /* usable text width, px                */
    int letter_space;  /* extra px between glyphs (usually 0)  */
    int tab_width;     /* a tab expands to this many spaces    */
    int max_lines;     /* lines per page                       */
} txt_layout_t;

typedef struct {
    size_t   off;      /* byte offset of the line's first character   */
    uint16_t len;      /* byte length, excluding the newline and any
                        * trailing space trimmed by wrapping          */
} txt_line_t;

/* Decode the UTF-8 sequence at `pos`. Stores the codepoint in *cp and returns
 * the number of bytes consumed; invalid bytes decode as U+FFFD consuming one
 * byte. Returns 0 when pos is at or past len. */
int txt_utf8_next(const char *s, size_t len, size_t pos, uint32_t *cp);

/* Step back to the start of the UTF-8 sequence containing `pos`. */
size_t txt_utf8_align(const char *s, size_t pos);

/* True for codepoints that may be broken between (CJK ideographs, kana and
 * fullwidth forms). */
int txt_is_cjk(uint32_t cp);

/* Lay out a single line beginning at `start`. Returns the offset at which the
 * next line begins (skipping the newline and any spaces eaten by wrapping);
 * *out_len receives the line's byte length. Returns `start` only at EOF. */
size_t txt_layout_line(const char *text, size_t len, size_t start,
                       const txt_layout_t *lay, uint16_t *out_len);

/* Lay out up to lay->max_lines lines from `start`. Fills lines[] (which must
 * hold max_lines entries) and *n_lines, and returns the offset where the next
 * page begins (== len at end of file). */
size_t txt_layout_page(const char *text, size_t len, size_t start,
                       const txt_layout_t *lay,
                       txt_line_t *lines, int *n_lines);

#ifdef __cplusplus
}
#endif

#endif /* __TEXT_LAYOUT_H__ */
