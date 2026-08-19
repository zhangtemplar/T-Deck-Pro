/**
 * @file      md_parse.c
 * @brief     Line-oriented Markdown block parser. See md_parse.h.
 */
#include "md_parse.h"

/* Length of the line starting at `p`, excluding its terminator, and the offset
 * where the next line begins. */
static size_t line_extent(const char *t, size_t len, size_t p, size_t *next)
{
    size_t e = p;
    while (e < len && t[e] != '\n' && t[e] != '\r') e++;
    size_t n = e;
    if (n < len && t[n] == '\r') n++;
    if (n < len && t[n] == '\n') n++;
    *next = n;
    return e - p;
}

static int is_space(char c) { return c == ' ' || c == '\t'; }

/* Word character for emphasis rules. Bytes >= 0x80 (UTF-8 continuation and
 * lead bytes) count as word characters so CJK behaves like text, not spaces. */
static int is_word(char c)
{
    unsigned char u = (unsigned char)c;
    return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') ||
           (u >= 'A' && u <= 'Z') || u >= 0x80;
}

/* A horizontal rule is three or more of -, * or _ with nothing else on the line. */
static int is_rule(const char *s, size_t n)
{
    char first = 0;
    int count = 0;
    for (size_t i = 0; i < n; i++) {
        if (is_space(s[i])) continue;
        if (s[i] != '-' && s[i] != '*' && s[i] != '_') return 0;
        if (first == 0) first = s[i];
        else if (s[i] != first) return 0;
        count++;
    }
    return count >= 3;
}

int md_parse(const char *text, size_t len, md_block_t *out, int max_blocks)
{
    int n = 0;
    size_t pos = 0;
    int in_fence = 0;

    while (pos < len && n < max_blocks) {
        size_t next;
        size_t llen = line_extent(text, len, pos, &next);
        const char *s = text + pos;

        /* Leading whitespace decides list nesting; two spaces per level. */
        size_t lead = 0;
        int cols = 0;
        while (lead < llen && is_space(s[lead])) { cols += (s[lead] == '\t') ? 4 : 1; lead++; }
        const char *b = s + lead;
        size_t blen = llen - lead;

        md_block_t *blk = &out[n];
        blk->indent = (uint8_t)(cols / 2);
        blk->ord = 0;

        /* A ``` fence toggles verbatim mode; the fence lines themselves are not
         * rendered. Everything between them is emitted untouched. */
        if (blen >= 3 && b[0] == '`' && b[1] == '`' && b[2] == '`') {
            in_fence = !in_fence;
            pos = next;
            continue;
        }
        if (in_fence) {
            blk->kind = MD_CODE;
            blk->off = pos;              /* keep original indentation */
            blk->len = (uint16_t)llen;
            blk->indent = 0;
            n++;
            pos = next;
            continue;
        }

        if (blen == 0) {
            blk->kind = MD_BLANK;
            blk->off = pos;
            blk->len = 0;
            n++;
            pos = next;
            continue;
        }

        if (is_rule(b, blen)) {
            blk->kind = MD_RULE;
            blk->off = pos;
            blk->len = 0;
            n++;
            pos = next;
            continue;
        }

        /* ATX headings: up to three levels, '#'s then a space. */
        if (b[0] == '#') {
            size_t h = 0;
            while (h < blen && b[h] == '#') h++;
            if (h >= 1 && h <= 6 && h < blen && is_space(b[h])) {
                size_t skip = h;
                while (skip < blen && is_space(b[skip])) skip++;
                blk->kind = (h == 1) ? MD_H1 : (h == 2) ? MD_H2 : MD_H3;
                blk->off = pos + lead + skip;
                blk->len = (uint16_t)(blen - skip);
                blk->indent = 0;
                n++;
                pos = next;
                continue;
            }
        }

        if (b[0] == '>') {
            size_t skip = 1;
            while (skip < blen && is_space(b[skip])) skip++;
            blk->kind = MD_QUOTE;
            blk->off = pos + lead + skip;
            blk->len = (uint16_t)(blen - skip);
            n++;
            pos = next;
            continue;
        }

        /* Bullets: "- ", "* ", "+ " (a bare "-" with no text is not a list). */
        if ((b[0] == '-' || b[0] == '*' || b[0] == '+') && blen >= 2 && is_space(b[1])) {
            size_t skip = 1;
            while (skip < blen && is_space(b[skip])) skip++;
            blk->kind = MD_BULLET;
            blk->off = pos + lead + skip;
            blk->len = (uint16_t)(blen - skip);
            n++;
            pos = next;
            continue;
        }

        /* Ordered: digits then '.' or ')' then a space. */
        if (b[0] >= '0' && b[0] <= '9') {
            size_t d = 0;
            unsigned val = 0;
            while (d < blen && b[d] >= '0' && b[d] <= '9' && d < 6) {
                val = val * 10 + (unsigned)(b[d] - '0');
                d++;
            }
            if (d < blen && (b[d] == '.' || b[d] == ')') && d + 1 < blen && is_space(b[d + 1])) {
                size_t skip = d + 1;
                while (skip < blen && is_space(b[skip])) skip++;
                blk->kind = MD_NUMBER;
                blk->ord = (uint16_t)val;
                blk->off = pos + lead + skip;
                blk->len = (uint16_t)(blen - skip);
                n++;
                pos = next;
                continue;
            }
        }

        blk->kind = MD_PARA;
        blk->off = pos + lead;
        blk->len = (uint16_t)blen;
        blk->indent = 0;
        n++;
        pos = next;
    }

    return n;
}

size_t md_strip_inline(const char *src, size_t src_len, char *dst)
{
    size_t i = 0, o = 0;

    while (i < src_len) {
        char c = src[i];

        /* Escaped punctuation: keep the character, drop the backslash. */
        if (c == '\\' && i + 1 < src_len) {
            dst[o++] = src[i + 1];
            i += 2;
            continue;
        }

        /* Emphasis and inline code markers carry no styling here, so drop them.
         * A lone marker surrounded by spaces is literal (e.g. "2 * 3"), and an
         * underscore *inside* a word is literal too, so snake_case identifiers
         * survive intact — CommonMark treats intra-word '_' the same way. */
        if (c == '*' || c == '_' || c == '`') {
            int before_sp = (i == 0) || is_space(src[i - 1]);
            int after_sp  = (i + 1 >= src_len) || is_space(src[i + 1]);
            int intra_word = 0;
            if (c == '_') {
                int prev_w = (i > 0) && is_word(src[i - 1]);
                int next_w = (i + 1 < src_len) && is_word(src[i + 1]);
                intra_word = prev_w && next_w;
            }
            if (!(before_sp && after_sp) && !intra_word) {
                while (i < src_len && src[i] == c) i++;   /* eat ** or __ too */
                continue;
            }
        }

        /* [label](target) -> label ; ![alt](src) -> alt */
        if (c == '!' && i + 1 < src_len && src[i + 1] == '[') {
            i++;
            continue;
        }
        if (c == '[') {
            size_t close = i + 1;
            while (close < src_len && src[close] != ']') close++;
            if (close < src_len && close + 1 < src_len && src[close + 1] == '(') {
                size_t paren = close + 2;
                while (paren < src_len && src[paren] != ')') paren++;
                if (paren < src_len) {
                    for (size_t k = i + 1; k < close; k++) dst[o++] = src[k];
                    i = paren + 1;
                    continue;
                }
            }
        }

        dst[o++] = c;
        i++;
    }

    return o;
}
