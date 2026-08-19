/**
 * @file      epub.c
 * @brief     EPUB container parsing. See epub.h.
 */
#include "epub.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------ tiny XML-ish scanning ------------------------------ */

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* Find `needle` in [p, end). Case-sensitive. */
static const char *find_in(const char *p, const char *end, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0 || (size_t)(end - p) < n) return NULL;
    for (const char *q = p; q + n <= end; q++) {
        if (memcmp(q, needle, n) == 0) return q;
    }
    return NULL;
}

/* Find the start of the next `<tag` element in [p, end), where the character
 * after the name must be whitespace or '>' so "<item" doesn't match "<itemref". */
static const char *find_tag(const char *p, const char *end, const char *tag)
{
    size_t n = strlen(tag);
    for (const char *q = p; q + n + 1 <= end; q++) {
        if (q[0] != '<' || memcmp(q + 1, tag, n) != 0) continue;
        char after = q[1 + n];
        if (is_ws(after) || after == '>' || after == '/') return q;
    }
    return NULL;
}

/* Extract attribute `attr` from the element starting at `tag`. The match must
 * be preceded by whitespace so `href` does not match `xlink:href`. */
static bool get_attr(const char *tag, const char *end, const char *attr,
                     char *out, size_t cap)
{
    const char *close = find_in(tag, end, ">");
    if (!close) close = end;
    size_t alen = strlen(attr);

    for (const char *p = tag + 1; p + alen + 2 < close; p++) {
        if (!is_ws(*p)) continue;
        const char *a = p + 1;
        if ((size_t)(close - a) <= alen) break;
        if (memcmp(a, attr, alen) != 0) continue;

        const char *q = a + alen;
        while (q < close && is_ws(*q)) q++;
        if (q >= close || *q != '=') continue;
        q++;
        while (q < close && is_ws(*q)) q++;
        if (q >= close || (*q != '"' && *q != '\'')) continue;
        char quote = *q++;
        const char *vend = q;
        while (vend < close && *vend != quote) vend++;
        if (vend >= close) return false;

        size_t vlen = (size_t)(vend - q);
        if (vlen > cap - 1) vlen = cap - 1;
        memcpy(out, q, vlen);
        out[vlen] = '\0';
        return true;
    }
    return false;
}

/* Percent-decode in place and cut any '#fragment'. EPUB hrefs are URIs. */
static void url_clean(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '#') break;
        if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

/* Join `dir` and a relative `href`, collapsing "./" and "../". */
static void path_resolve(const char *dir, const char *href, char *out, size_t cap)
{
    char tmp[EPUB_PATH_LEN * 2];
    if (href[0] == '/') snprintf(tmp, sizeof(tmp), "%s", href + 1);
    else if (dir[0])    snprintf(tmp, sizeof(tmp), "%s/%s", dir, href);
    else                snprintf(tmp, sizeof(tmp), "%s", href);

    /* Normalise by rebuilding from path components. */
    char *segs[64];
    int n = 0;
    for (char *tok = strtok(tmp, "/"); tok && n < 64; tok = strtok(NULL, "/")) {
        if (!strcmp(tok, ".") || !*tok) continue;
        if (!strcmp(tok, "..")) { if (n > 0) n--; continue; }
        segs[n++] = tok;
    }

    size_t o = 0;
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        int w = snprintf(out + o, cap - o, "%s%s", i ? "/" : "", segs[i]);
        if (w < 0 || (size_t)w >= cap - o) break;
        o += (size_t)w;
    }
}

/* ------------------------------ table of contents ------------------------------ */

/* Copy element text (up to the next '<') into out, collapsing whitespace. */
static void copy_text(const char *p, const char *end, char *out, size_t cap)
{
    size_t o = 0;
    bool sp = false;
    for (; p < end && *p != '<' && o + 1 < cap; p++) {
        if (is_ws(*p)) { sp = (o > 0); continue; }
        if (sp) { out[o++] = ' '; sp = false; if (o + 1 >= cap) break; }
        out[o++] = *p;
    }
    out[o] = '\0';
}

/* Record `title` for whichever chapter `src` refers to. */
static void set_chapter_title(epub_t *e, const char *toc_dir, const char *src,
                              const char *title)
{
    if (!title[0]) return;
    char href[EPUB_PATH_LEN];
    snprintf(href, sizeof(href), "%s", src);
    url_clean(href);                       /* also drops any #fragment */
    if (!href[0]) return;

    char full[EPUB_PATH_LEN];
    path_resolve(toc_dir, href, full, sizeof(full));
    for (int i = 0; i < e->nchap; i++) {
        if (strcmp(e->chap[i], full) == 0) {
            if (!e->chap_title[i][0])
                snprintf(e->chap_title[i], EPUB_TITLE_LEN, "%s", title);
            return;
        }
    }
}

/* Pull chapter names out of an EPUB 2 .ncx or an EPUB 3 nav document. Titles
 * are cosmetic — a book with no usable TOC just falls back to file names. */
static void parse_toc(epub_t *e, const char *toc_path, const uint8_t *doc, size_t len)
{
    char toc_dir[EPUB_PATH_LEN] = "";
    const char *slash = strrchr(toc_path, '/');
    if (slash) {
        size_t dl = (size_t)(slash - toc_path);
        if (dl >= sizeof(toc_dir)) dl = sizeof(toc_dir) - 1;
        memcpy(toc_dir, toc_path, dl);
        toc_dir[dl] = '\0';
    }

    const char *b = (const char *)doc, *end = b + len;

    /* EPUB 2: <navPoint><navLabel><text>Title</text></navLabel>
     *                   <content src="chapter.xhtml"/></navPoint> */
    for (const char *np = find_tag(b, end, "navPoint"); np;
         np = find_tag(np + 1, end, "navPoint")) {
        const char *stop = find_tag(np + 1, end, "navPoint");
        if (!stop) stop = end;

        char title[EPUB_TITLE_LEN] = "";
        const char *tx = find_tag(np, stop, "text");
        if (tx) {
            const char *gt = find_in(tx, stop, ">");
            if (gt) copy_text(gt + 1, stop, title, sizeof(title));
        }
        const char *ct = find_tag(np, stop, "content");
        char src[EPUB_PATH_LEN] = "";
        if (ct) get_attr(ct, stop, "src", src, sizeof(src));
        if (src[0]) set_chapter_title(e, toc_dir, src, title);
    }

    /* EPUB 3: <nav ...><ol><li><a href="chapter.xhtml">Title</a></li>... */
    for (const char *a = find_tag(b, end, "a"); a; a = find_tag(a + 1, end, "a")) {
        char href[EPUB_PATH_LEN] = "";
        if (!get_attr(a, end, "href", href, sizeof(href))) continue;
        const char *gt = find_in(a, end, ">");
        if (!gt) continue;
        char title[EPUB_TITLE_LEN] = "";
        copy_text(gt + 1, end, title, sizeof(title));
        set_chapter_title(e, toc_dir, href, title);
    }
}

/* Locate the TOC document via the manifest and hand it to parse_toc. */
static void load_toc(epub_t *e, const char *man, const char *man_end)
{
    char toc_href[EPUB_PATH_LEN] = "";

    for (const char *it = find_tag(man, man_end, "item"); it;
         it = find_tag(it + 1, man_end, "item")) {
        char media[64] = "", props[96] = "";
        get_attr(it, man_end, "media-type", media, sizeof(media));
        get_attr(it, man_end, "properties", props, sizeof(props));
        bool is_ncx = !strcmp(media, "application/x-dtbncx+xml");
        bool is_nav = strstr(props, "nav") != NULL;
        if (!is_ncx && !is_nav) continue;
        if (get_attr(it, man_end, "href", toc_href, sizeof(toc_href))) {
            url_clean(toc_href);
            if (is_ncx) break;             /* prefer the ncx; keep looking otherwise */
        }
    }
    if (!toc_href[0]) return;

    char full[EPUB_PATH_LEN];
    path_resolve(e->opf_dir, toc_href, full, sizeof(full));
    int zi = zip_find(&e->zip, full);
    if (zi < 0) return;

    uint8_t *doc = NULL;
    size_t dlen = 0;
    if (zip_extract(&e->zip, zi, &doc, &dlen) != 0) return;
    parse_toc(e, full, doc, dlen);
    free(doc);
}

/* ------------------------------ public ------------------------------ */

const char *epub_err_text(int err)
{
    switch (err) {
    case EPUB_ERR_DRM:    return "DRM-protected book";
    case EPUB_ERR_FORMAT: return "malformed EPUB";
    case EPUB_ERR_MEMORY: return "out of memory";
    case EPUB_ERR_ZIP:    return "unreadable archive";
    default:              return "error";
    }
}

int epub_open(epub_t *e, const uint8_t *data, size_t len)
{
    memset(e, 0, sizeof(*e));

    if (zip_open(&e->zip, data, len) != 0) return EPUB_ERR_ZIP;

    /* Encrypted books would otherwise decompress into noise. */
    if (zip_find(&e->zip, "META-INF/encryption.xml") >= 0) return EPUB_ERR_DRM;

    /* container.xml -> the package document's path */
    int idx = zip_find(&e->zip, "META-INF/container.xml");
    if (idx < 0) return EPUB_ERR_FORMAT;
    uint8_t *cont = NULL;
    size_t clen = 0;
    if (zip_extract(&e->zip, idx, &cont, &clen) != 0) return EPUB_ERR_FORMAT;

    char opf_path[EPUB_PATH_LEN] = "";
    const char *cbeg = (const char *)cont, *cend = cbeg + clen;
    const char *rf = find_tag(cbeg, cend, "rootfile");
    if (rf) get_attr(rf, cend, "full-path", opf_path, sizeof(opf_path));
    free(cont);
    if (!opf_path[0]) return EPUB_ERR_FORMAT;
    url_clean(opf_path);

    /* Remember the .opf's directory: manifest hrefs are relative to it. */
    const char *slash = strrchr(opf_path, '/');
    if (slash) {
        size_t dl = (size_t)(slash - opf_path);
        if (dl >= sizeof(e->opf_dir)) dl = sizeof(e->opf_dir) - 1;
        memcpy(e->opf_dir, opf_path, dl);
        e->opf_dir[dl] = '\0';
    }

    idx = zip_find(&e->zip, opf_path);
    if (idx < 0) return EPUB_ERR_FORMAT;
    uint8_t *opf = NULL;
    size_t olen = 0;
    if (zip_extract(&e->zip, idx, &opf, &olen) != 0) return EPUB_ERR_FORMAT;

    const char *obeg = (const char *)opf, *oend = obeg + olen;

    /* Title, for the chapter list header. */
    const char *t = find_tag(obeg, oend, "dc:title");
    if (!t) t = find_tag(obeg, oend, "title");
    if (t) {
        const char *gt = find_in(t, oend, ">");
        if (gt) {
            gt++;
            const char *lt = find_in(gt, oend, "<");
            if (lt) {
                size_t n = (size_t)(lt - gt);
                if (n > sizeof(e->title) - 1) n = sizeof(e->title) - 1;
                memcpy(e->title, gt, n);
                e->title[n] = '\0';
            }
        }
    }

    /* Locate manifest and spine sections. */
    const char *man = find_tag(obeg, oend, "manifest");
    const char *man_end = man ? find_in(man, oend, "</manifest") : NULL;
    const char *spn = find_tag(obeg, oend, "spine");
    const char *spn_end = spn ? find_in(spn, oend, "</spine") : NULL;
    if (!man || !man_end || !spn || !spn_end) { free(opf); return EPUB_ERR_FORMAT; }

    e->chap = (char (*)[EPUB_PATH_LEN])calloc(EPUB_MAX_CHAPTERS, EPUB_PATH_LEN);
    e->chap_title = (char (*)[EPUB_TITLE_LEN])calloc(EPUB_MAX_CHAPTERS, EPUB_TITLE_LEN);
    if (!e->chap || !e->chap_title) { free(opf); epub_close(e); return EPUB_ERR_MEMORY; }

    /* Walk the spine; for each idref look the href up in the manifest. The
     * manifest is scanned per item rather than indexed up front — books have a
     * few hundred entries at most, and this avoids a large temporary table. */
    for (const char *ir = find_tag(spn, spn_end, "itemref");
         ir && e->nchap < EPUB_MAX_CHAPTERS;
         ir = find_tag(ir + 1, spn_end, "itemref")) {

        char idref[96];
        if (!get_attr(ir, spn_end, "idref", idref, sizeof(idref))) continue;

        for (const char *it = find_tag(man, man_end, "item");
             it;
             it = find_tag(it + 1, man_end, "item")) {

            char id[96];
            if (!get_attr(it, man_end, "id", id, sizeof(id))) continue;
            if (strcmp(id, idref) != 0) continue;

            char href[EPUB_PATH_LEN];
            if (!get_attr(it, man_end, "href", href, sizeof(href))) break;
            url_clean(href);
            path_resolve(e->opf_dir, href, e->chap[e->nchap], EPUB_PATH_LEN);

            /* Drop anything the archive doesn't actually contain. */
            if (zip_find(&e->zip, e->chap[e->nchap]) >= 0) e->nchap++;
            break;
        }
    }

    if (e->nchap == 0) { free(opf); epub_close(e); return EPUB_ERR_FORMAT; }

    /* Chapter names for the list; purely cosmetic, so failures are ignored. */
    load_toc(e, man, man_end);

    free(opf);
    return 0;
}

void epub_close(epub_t *e)
{
    if (e->chap)       { free(e->chap);       e->chap = NULL; }
    if (e->chap_title) { free(e->chap_title); e->chap_title = NULL; }
    e->nchap = 0;
}

int epub_chapter(const epub_t *e, int idx, uint8_t **out, size_t *len)
{
    if (idx < 0 || idx >= e->nchap) return EPUB_ERR_FORMAT;
    int zi = zip_find(&e->zip, e->chap[idx]);
    if (zi < 0) return EPUB_ERR_FORMAT;
    return zip_extract(&e->zip, zi, out, len) == 0 ? 0 : EPUB_ERR_FORMAT;
}
