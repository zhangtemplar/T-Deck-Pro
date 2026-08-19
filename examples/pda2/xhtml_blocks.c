/**
 * @file      xhtml_blocks.c
 * @brief     XHTML -> md_block_t conversion. See xhtml_blocks.h.
 */
#include "xhtml_blocks.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>

#define LIST_DEPTH_MAX 8

typedef struct {
    char     *text;
    size_t    cap;
    size_t    len;

    md_block_t *blocks;
    int         max_blocks;
    int         nblocks;

    /* Block being accumulated. */
    md_kind_t kind;
    size_t    start;        /* offset in text where this block began */
    uint8_t   indent;
    uint16_t  ord;
    bool      open;

    /* <pre> keeps whitespace; everywhere else runs of it collapse to a space. */
    int  pre_depth;
    int  skip_depth;        /* inside <head>/<script>/<style>                */
    int  quote_depth;

    /* Ordered/unordered list nesting; ord counts items in the current list. */
    struct { bool ordered; uint16_t counter; } lists[LIST_DEPTH_MAX];
    int list_depth;
} ctx_t;

/* ------------------------------ text emission ------------------------------ */

static void put_ch(ctx_t *c, char ch)
{
    if (c->len + 1 < c->cap) c->text[c->len++] = ch;
}

static void put_str(ctx_t *c, const char *s)
{
    while (*s) put_ch(c, *s++);
}

/* True when the block currently has no visible characters yet. */
static bool block_empty(const ctx_t *c) { return c->len <= c->start; }

static char last_ch(const ctx_t *c) { return c->len > c->start ? c->text[c->len - 1] : 0; }

/* ------------------------------ block boundaries ------------------------------ */

static void block_flush(ctx_t *c)
{
    if (!c->open) return;
    c->open = false;

    /* Trim trailing space introduced by whitespace collapsing. */
    while (c->len > c->start && c->text[c->len - 1] == ' ') c->len--;

    if (block_empty(c)) {
        /* Nothing in it: drop the block, but keep rules/blank spacers. */
        if (c->kind != MD_RULE && c->kind != MD_BLANK) return;
    }
    if (c->nblocks >= c->max_blocks) return;

    md_block_t *b = &c->blocks[c->nblocks++];
    b->kind   = c->kind;
    b->off    = c->start;
    b->len    = (uint16_t)((c->len - c->start) > 0xFFFF ? 0xFFFF : (c->len - c->start));
    b->indent = c->indent;
    b->ord    = c->ord;
}

static void block_begin(ctx_t *c, md_kind_t kind, uint8_t indent, uint16_t ord)
{
    block_flush(c);
    c->kind   = kind;
    c->indent = indent;
    c->ord    = ord;
    c->start  = c->len;
    c->open   = true;
}

/* A paragraph-ish default, used when text appears outside any known block. */
static void ensure_open(ctx_t *c)
{
    if (!c->open) block_begin(c, c->quote_depth ? MD_QUOTE : MD_PARA, 0, 0);
}

/* ------------------------------ entities ------------------------------ */

static void put_utf8(ctx_t *c, uint32_t cp)
{
    if (cp < 0x80) { put_ch(c, (char)cp); }
    else if (cp < 0x800) {
        put_ch(c, (char)(0xC0 | (cp >> 6)));
        put_ch(c, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        put_ch(c, (char)(0xE0 | (cp >> 12)));
        put_ch(c, (char)(0x80 | ((cp >> 6) & 0x3F)));
        put_ch(c, (char)(0x80 | (cp & 0x3F)));
    } else {
        put_ch(c, (char)(0xF0 | (cp >> 18)));
        put_ch(c, (char)(0x80 | ((cp >> 12) & 0x3F)));
        put_ch(c, (char)(0x80 | ((cp >> 6) & 0x3F)));
        put_ch(c, (char)(0x80 | (cp & 0x3F)));
    }
}

static const struct { const char *name; uint32_t cp; } k_entities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
    {"nbsp", ' '}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026},
    {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
    {"middot", 0x00B7}, {"bull", 0x2022}, {"deg", 0x00B0}, {"copy", 0x00A9},
    {"trade", 0x2122}, {"reg", 0x00AE}, {"eacute", 0x00E9}, {"egrave", 0x00E8},
    {"agrave", 0x00E0}, {"uuml", 0x00FC}, {"ouml", 0x00F6}, {"auml", 0x00E4},
    {"szlig", 0x00DF}, {"times", 0x00D7}, {"laquo", 0x00AB}, {"raquo", 0x00BB},
};

/* Decode the entity starting at `p` (which points just past '&'). Returns the
 * offset of the character after it, or 0 if this is not a valid entity. */
static size_t decode_entity(ctx_t *c, const char *p, const char *end)
{
    const char *semi = p;
    while (semi < end && semi - p < 12 && *semi != ';' && !isspace((unsigned char)*semi)) semi++;
    if (semi >= end || *semi != ';') return 0;

    size_t n = (size_t)(semi - p);
    if (n == 0) return 0;

    if (p[0] == '#') {
        uint32_t cp = 0;
        if (n > 1 && (p[1] == 'x' || p[1] == 'X')) {
            for (size_t i = 2; i < n; i++) {
                if (!isxdigit((unsigned char)p[i])) return 0;
                char d = p[i];
                cp = cp * 16 + (uint32_t)(isdigit((unsigned char)d) ? d - '0'
                                          : (tolower(d) - 'a' + 10));
            }
        } else {
            for (size_t i = 1; i < n; i++) {
                if (!isdigit((unsigned char)p[i])) return 0;
                cp = cp * 10 + (uint32_t)(p[i] - '0');
            }
        }
        if (cp == 0 || cp > 0x10FFFF) return 0;
        put_utf8(c, cp);
        return n + 1;
    }

    for (size_t i = 0; i < sizeof(k_entities) / sizeof(k_entities[0]); i++) {
        if (strlen(k_entities[i].name) == n && memcmp(p, k_entities[i].name, n) == 0) {
            put_utf8(c, k_entities[i].cp);
            return n + 1;
        }
    }
    return 0;
}

/* ------------------------------ tag handling ------------------------------ */

static bool name_eq(const char *p, size_t n, const char *lit)
{
    size_t l = strlen(lit);
    if (n != l) return false;
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)p[i]) != lit[i]) return false;
    }
    return true;
}

/* Pull the alt/title text out of an <img> so the reader sees something. */
static void emit_img_alt(ctx_t *c, const char *tag, const char *end)
{
    static const char *attrs[] = { "alt=", "title=" };
    for (int a = 0; a < 2; a++) {
        size_t al = strlen(attrs[a]);
        for (const char *p = tag; p + al < end; p++) {
            if (memcmp(p, attrs[a], al) != 0) continue;
            const char *q = p + al;
            if (q >= end || (*q != '"' && *q != '\'')) continue;
            char quote = *q++;
            const char *vend = q;
            while (vend < end && *vend != quote) vend++;
            if (vend >= end || vend == q) break;      /* empty alt: fall through */
            ensure_open(c);
            if (last_ch(c) && last_ch(c) != ' ') put_ch(c, ' ');
            put_ch(c, '[');
            for (const char *k = q; k < vend; k++) put_ch(c, *k);
            put_ch(c, ']');
            return;
        }
    }
    ensure_open(c);
    if (last_ch(c) && last_ch(c) != ' ') put_ch(c, ' ');
    put_str(c, "[image]");
}

int xhtml_to_blocks(const char *xml, size_t xml_len,
                    char *text_out, size_t text_cap, size_t *text_len,
                    md_block_t *blocks, int max_blocks)
{
    ctx_t c;
    memset(&c, 0, sizeof(c));
    c.text = text_out;
    c.cap = text_cap;
    c.blocks = blocks;
    c.max_blocks = max_blocks;
    c.kind = MD_PARA;

    const char *p = xml, *end = xml + xml_len;

    while (p < end) {
        if (*p == '<') {
            /* Comments, CDATA and declarations. */
            if (end - p >= 4 && memcmp(p, "<!--", 4) == 0) {
                const char *e2 = p + 4;
                while (e2 + 3 <= end && memcmp(e2, "-->", 3) != 0) e2++;
                p = (e2 + 3 <= end) ? e2 + 3 : end;
                continue;
            }
            if (end - p >= 2 && (p[1] == '?' || p[1] == '!')) {
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                continue;
            }

            const char *q = p + 1;
            bool closing = (q < end && *q == '/');
            if (closing) q++;
            const char *nstart = q;
            while (q < end && (isalnum((unsigned char)*q) || *q == ':' || *q == '-')) q++;
            size_t nlen = (size_t)(q - nstart);

            const char *tag_end = q;
            while (tag_end < end && *tag_end != '>') tag_end++;
            const char *next = (tag_end < end) ? tag_end + 1 : end;
            /* "<x/>" is self-closing; "<x>" is not — note tag_end == q for a
             * bare tag with no attributes, so the emptiness must be handled. */
            bool self_closing = (tag_end > q && tag_end[-1] == '/');

            if (nlen == 0) { p = next; continue; }
            const char *nm = nstart;

            /* Sections whose contents must not appear as text. */
            if (name_eq(nm, nlen, "head") || name_eq(nm, nlen, "script") ||
                name_eq(nm, nlen, "style") || name_eq(nm, nlen, "svg") ||
                name_eq(nm, nlen, "title")) {
                if (closing) { if (c.skip_depth > 0) c.skip_depth--; }
                else if (!self_closing) c.skip_depth++;
                p = next;
                continue;
            }
            if (c.skip_depth > 0) { p = next; continue; }

            if (name_eq(nm, nlen, "br")) {
                block_flush(&c);
                p = next;
                continue;
            }
            if (name_eq(nm, nlen, "hr")) {
                block_begin(&c, MD_RULE, 0, 0);
                block_flush(&c);
                p = next;
                continue;
            }
            if (name_eq(nm, nlen, "img") || name_eq(nm, nlen, "image")) {
                if (!closing) emit_img_alt(&c, p, tag_end);
                p = next;
                continue;
            }

            if (name_eq(nm, nlen, "pre")) {
                if (closing) { if (c.pre_depth > 0) c.pre_depth--; block_flush(&c); }
                else { block_begin(&c, MD_CODE, 0, 0); c.pre_depth++; }
                p = next;
                continue;
            }

            if (name_eq(nm, nlen, "blockquote")) {
                if (closing) { if (c.quote_depth > 0) c.quote_depth--; }
                else c.quote_depth++;
                block_flush(&c);
                p = next;
                continue;
            }

            if (name_eq(nm, nlen, "ul") || name_eq(nm, nlen, "ol")) {
                block_flush(&c);
                if (closing) { if (c.list_depth > 0) c.list_depth--; }
                else if (c.list_depth < LIST_DEPTH_MAX) {
                    c.lists[c.list_depth].ordered = name_eq(nm, nlen, "ol");
                    c.lists[c.list_depth].counter = 0;
                    c.list_depth++;
                }
                p = next;
                continue;
            }

            if (name_eq(nm, nlen, "li")) {
                if (closing) { block_flush(&c); p = next; continue; }
                int d = c.list_depth > 0 ? c.list_depth - 1 : 0;
                bool ordered = c.list_depth > 0 && c.lists[d].ordered;
                uint16_t ord = 0;
                if (ordered) ord = ++c.lists[d].counter;
                block_begin(&c, ordered ? MD_NUMBER : MD_BULLET, (uint8_t)d, ord);
                p = next;
                continue;
            }

            if (nlen == 2 && tolower((unsigned char)nm[0]) == 'h' &&
                nm[1] >= '1' && nm[1] <= '6') {
                if (closing) block_flush(&c);
                else {
                    md_kind_t k = (nm[1] == '1') ? MD_H1 : (nm[1] == '2') ? MD_H2 : MD_H3;
                    block_begin(&c, k, 0, 0);
                }
                p = next;
                continue;
            }

            if (name_eq(nm, nlen, "p")   || name_eq(nm, nlen, "div") ||
                name_eq(nm, nlen, "tr")  || name_eq(nm, nlen, "section") ||
                name_eq(nm, nlen, "article") || name_eq(nm, nlen, "body") ||
                name_eq(nm, nlen, "td")  || name_eq(nm, nlen, "th") ||
                name_eq(nm, nlen, "figcaption")) {
                block_flush(&c);
                if (!closing && (name_eq(nm, nlen, "td") || name_eq(nm, nlen, "th"))) {
                    /* Cells run together on one line rather than each becoming
                     * a paragraph; a real table layout is out of scope. */
                    ensure_open(&c);
                    if (last_ch(&c) && last_ch(&c) != ' ') put_ch(&c, ' ');
                }
                p = next;
                continue;
            }

            /* Everything else (em, strong, span, a, ...) contributes no
             * structure and no styling we can express — skip the tag itself. */
            p = next;
            continue;
        }

        /* ---- character data ---- */
        if (c.skip_depth > 0) { p++; continue; }

        if (*p == '&') {
            size_t used = decode_entity(&c, p + 1, end);
            if (used) { ensure_open(&c); p += 1 + used; continue; }
            ensure_open(&c);
            put_ch(&c, '&');
            p++;
            continue;
        }

        if (c.pre_depth > 0) {
            if (*p == '\n') { block_flush(&c); block_begin(&c, MD_CODE, 0, 0); }
            else            { ensure_open(&c); put_ch(&c, *p); }
            p++;
            continue;
        }

        if (isspace((unsigned char)*p)) {
            /* Collapse runs of whitespace; never start a block with one. */
            while (p < end && isspace((unsigned char)*p)) p++;
            if (c.open && !block_empty(&c) && last_ch(&c) != ' ') put_ch(&c, ' ');
            continue;
        }

        ensure_open(&c);
        put_ch(&c, *p);
        p++;
    }

    block_flush(&c);

    if (c.len < c.cap) c.text[c.len] = '\0';
    *text_len = c.len;
    return c.nblocks;
}
