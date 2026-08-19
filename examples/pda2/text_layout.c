/**
 * @file      text_layout.c
 * @brief     UTF-8 word wrapping / pagination. See text_layout.h.
 */
#include "text_layout.h"

/* Hard ceiling on a single line's byte length. Control characters report a zero
 * advance (LVGL only substitutes a placeholder for codepoints >= 0x20), so a
 * file full of them — a binary opened as .txt, or embedded NULs — would never
 * reach the wrap width and the line would run to EOF, overflowing the uint16_t
 * length. A real text line is a couple of hundred bytes at most. */
#define TXT_MAX_LINE_BYTES 2048

int txt_utf8_next(const char *s, size_t len, size_t pos, uint32_t *cp)
{
    if (pos >= len) { *cp = 0; return 0; }

    const unsigned char *p = (const unsigned char *)s + pos;
    size_t avail = len - pos;
    unsigned char c = p[0];

    if (c < 0x80) { *cp = c; return 1; }

    int need;
    uint32_t v;
    if ((c & 0xE0) == 0xC0)      { need = 2; v = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { need = 3; v = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { need = 4; v = c & 0x07; }
    else                         { *cp = 0xFFFD; return 1; }   /* stray continuation */

    if ((size_t)need > avail) { *cp = 0xFFFD; return 1; }
    for (int i = 1; i < need; i++) {
        if ((p[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; } /* truncated */
        v = (v << 6) | (p[i] & 0x3F);
    }
    *cp = v;
    return need;
}

size_t txt_utf8_align(const char *s, size_t pos)
{
    const unsigned char *p = (const unsigned char *)s;
    while (pos > 0 && (p[pos] & 0xC0) == 0x80) pos--;
    return pos;
}

int txt_is_cjk(uint32_t cp)
{
    return (cp >= 0x2E80 && cp <= 0x9FFF) ||   /* radicals, kana, ideographs   */
           (cp >= 0xF900 && cp <= 0xFAFF) ||   /* compatibility ideographs     */
           (cp >= 0xFF00 && cp <= 0xFF60) ||   /* fullwidth forms              */
           (cp >= 0x3000 && cp <= 0x303F);     /* CJK punctuation              */
}

/* Punctuation that may not begin a line, so no break immediately before it. */
static int no_break_before(uint32_t cp)
{
    switch (cp) {
    case 0x3001: case 0x3002:                       /* 、 。               */
    case 0xFF0C: case 0xFF0E: case 0xFF1A: case 0xFF1B: /* ， ． ： ；      */
    case 0xFF01: case 0xFF1F:                       /* ！ ？               */
    case 0xFF09: case 0x3009: case 0x300B:          /* ） 〉 》            */
    case 0x300D: case 0x300F: case 0x3011: case 0x3015: /* 」 』 】 〕     */
    case 0x2019: case 0x201D:                       /* ’ ”                */
    case 0xFF3D: case 0xFF5D:                       /* ］ ｝               */
        return 1;
    default:
        return 0;
    }
}

/* Punctuation that may not end a line, so no break immediately after it. */
static int no_break_after(uint32_t cp)
{
    switch (cp) {
    case 0xFF08: case 0x3008: case 0x300A:          /* （ 〈 《            */
    case 0x300C: case 0x300E: case 0x3010: case 0x3014: /* 「 『 【 〔     */
    case 0x2018: case 0x201C:                       /* ‘ “                */
    case 0xFF3B: case 0xFF5B:                       /* ［ ｛               */
        return 1;
    default:
        return 0;
    }
}

/* ASCII characters after which a line may wrap. */
static int ascii_break_after(uint32_t cp)
{
    switch (cp) {
    case ',': case '.': case ';': case ':': case '!': case '?':
    case ')': case ']': case '}': case '-': case '/':
        return 1;
    default:
        return 0;
    }
}

static int adv_of(const txt_layout_t *lay, uint32_t cp)
{
    if (cp == '\t') return lay->tab_width * lay->advance(' ', lay->ctx);
    return lay->advance(cp, lay->ctx);
}

size_t txt_layout_line(const char *text, size_t len, size_t start,
                       const txt_layout_t *lay, uint16_t *out_len)
{
    *out_len = 0;
    if (start >= len) return start;

    size_t pos = start;
    int    w = 0;
    size_t brk = 0;        /* offset just after the last break opportunity */
    size_t brk_end = 0;    /* line end to use for that break (trailing space trimmed) */
    uint32_t prev = 0;

    while (pos < len) {
        uint32_t cp;
        int n = txt_utf8_next(text, len, pos, &cp);
        if (n == 0) break;

        /* End the line on any of the three conventions (LF, CRLF, lone CR) and
         * leave the terminator out of the returned span — a stray CR left
         * inside a line would be handed to the renderer as a glyph. */
        if (cp == '\r') {
            *out_len = (uint16_t)(pos - start);
            size_t next = pos + n;
            if (next < len && text[next] == '\n') next++;
            return next;
        }
        if (cp == '\n') {
            *out_len = (uint16_t)(pos - start);
            return pos + n;
        }

        int cw = adv_of(lay, cp) + lay->letter_space;

        /* Does this glyph overflow? Always place at least one, so a single
         * oversized glyph can never wedge the layout. */
        if (w + cw > lay->width && pos > start) {
            if (brk > start) {
                *out_len = (uint16_t)(brk_end - start);
                /* Swallow spaces at the wrap point so the next line is flush. */
                size_t next = brk;
                while (next < len && text[next] == ' ') next++;
                return next;
            }
            *out_len = (uint16_t)(pos - start);        /* hard break mid-word */
            return pos;
        }

        w += cw;
        pos += n;

        if (pos - start >= TXT_MAX_LINE_BYTES) {   /* see TXT_MAX_LINE_BYTES */
            *out_len = (uint16_t)(pos - start);
            return pos;
        }

        /* Record where this glyph allows a following break. */
        int can_break = 0;
        if (cp == ' ') {
            can_break = 1;
        } else if (txt_is_cjk(cp) || ascii_break_after(cp)) {
            can_break = !no_break_after(cp);
        }
        if (can_break) {
            uint32_t nxt = 0;
            if (pos < len) txt_utf8_next(text, len, pos, &nxt);
            if (!nxt || !no_break_before(nxt)) {
                brk = pos;
                /* A wrap at a space drops the space itself from the line. */
                brk_end = (cp == ' ') ? pos - 1 : pos;
            }
        }
        prev = cp;
        (void)prev;
    }

    *out_len = (uint16_t)(pos - start);
    return pos;
}

size_t txt_layout_page(const char *text, size_t len, size_t start,
                       const txt_layout_t *lay,
                       txt_line_t *lines, int *n_lines)
{
    int n = 0;
    size_t pos = start;

    while (n < lay->max_lines && pos < len) {
        uint16_t ll = 0;
        size_t next = txt_layout_line(text, len, pos, lay, &ll);
        lines[n].off = pos;
        lines[n].len = ll;
        n++;
        if (next == pos) break;        /* no progress: bail rather than spin */
        pos = next;
    }

    *n_lines = n;
    return pos;
}
