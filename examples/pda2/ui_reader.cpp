/**
 * @file      ui_reader.cpp
 * @brief     Plain-text reader (.txt / .md / .log) with auto-resume.
 *
 * Page 1: the text, wrapped and paginated by text_layout.c (LVGL's own breaker
 *         can't wrap Chinese, so we pre-wrap and hand the label finished lines).
 * Page 2: a file browser for picking a book.
 *
 * Pagination is incremental rather than a full pre-scan: laying out a 1 MB book
 * up front would stall for seconds, so we lay out one page at a time and push
 * each page's start offset on a stack, which makes going back exact. After a
 * resume — where there is no stack yet — the previous page is recovered by
 * re-laying out from a line boundary a little way back.
 *
 * The reading position of each book is remembered in a small index on the card.
 */
#include "Arduino.h"
#include "ui_deckpro.h"
#include "ui_deckpro_port.h"
#include "factory.h"
#include "text_layout.h"
#include "cjk_font.h"
#include "src/assets.h"
#include "utilities.h"
#include <SD.h>

extern void shared_spi_lock(void);
extern void shared_spi_unlock(void);
extern void shared_spi_prepare_device(int cs_pin);

#define RD_PAGE_COUNT    2
#define RD_START_DIR     "/books"
#define RD_POS_FILE      "/books/.reader"
#define RD_MAX_ENTRIES   128
#define RD_NAME_LEN      48
#define RD_PATH_LEN      96
#define RD_MAX_FILE      (4u * 1024u * 1024u)
#define RD_MAX_LINES     20          /* upper bound; actual count is computed */
#define RD_MAX_STACK     2048        /* page starts remembered for going back */
#define RD_TEXT_W        228
#define RD_TEXT_X        6
#define RD_TEXT_Y        30
#define RD_TEXT_H        258
/* How far back to re-lay-out when the page stack can't answer (resume case). */
#define RD_BACK_PROBE    6000

static lv_obj_t *pages[RD_PAGE_COUNT] = {};
static lv_obj_t *page_ind = NULL;
static int rd_page = 0;
static bool rd_kbd_active = false;

/* Page 1 */
static lv_obj_t *text_label = NULL;
static lv_obj_t *status_label = NULL;

/* Page 2 */
static lv_obj_t *browser_list = NULL;
static lv_obj_t *browser_path_lbl = NULL;

/* Book */
static char  *g_text = NULL;         /* whole file in PSRAM, NUL-terminated */
static size_t g_len = 0;
static char   g_path[RD_PATH_LEN] = "";
static size_t g_pos = 0;             /* byte offset of the current page start */
static bool   g_big_font = false;

/* Visited page starts, for exact backward paging. */
static size_t *g_stack = NULL;
static int     g_stack_n = 0;

/* Directory listing */
struct entry { char name[RD_NAME_LEN]; bool is_dir; };
static entry entries[RD_MAX_ENTRIES];
static int entry_count = 0;
static char cur_dir[RD_PATH_LEN] = RD_START_DIR;

static void show_rd_page(int pg);
static void refresh_browser(void);
static void render_page(void);

/* ---- helpers ---- */

static const char *ext_of(const char *n) { const char *d = strrchr(n, '.'); return d ? d + 1 : ""; }

static bool ext_eq(const char *a, const char *b)
{
    while (*a && *b) { if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

static bool is_text_name(const char *n)
{
    const char *e = ext_of(n);
    return ext_eq(e, "txt") || ext_eq(e, "md") || ext_eq(e, "log") ||
           ext_eq(e, "csv") || ext_eq(e, "json");
}

static void path_join_into(char *dst, size_t cap, const char *dir, const char *leaf)
{
    if (!strcmp(dir, "/")) snprintf(dst, cap, "/%s", leaf);
    else                   snprintf(dst, cap, "%s/%s", dir, leaf);
}

static void parent_dir_into(char *dst, size_t cap, const char *p)
{
    const char *s = strrchr(p, '/');
    if (!s || s == p) { snprintf(dst, cap, "/"); return; }
    size_t n = (size_t)(s - p);
    if (n >= cap) n = cap - 1;
    memcpy(dst, p, n);
    dst[n] = '\0';
}

static const lv_font_t *cur_font(void)
{
    return g_big_font ? &g_font_cn_large : &g_font_cn;
}

/* ---- layout glue ---- */

static int reader_advance(uint32_t cp, void *ctx)
{
    return lv_font_get_glyph_width((const lv_font_t *)ctx, cp, 0);
}

static void make_layout(txt_layout_t *lay)
{
    const lv_font_t *f = cur_font();
    lay->advance     = reader_advance;
    lay->ctx         = (void *)f;
    lay->width       = RD_TEXT_W;
    lay->letter_space = 0;
    lay->tab_width   = 4;
    lay->max_lines   = RD_TEXT_H / (lv_font_get_line_height(f) + 2);
    if (lay->max_lines < 1) lay->max_lines = 1;
    if (lay->max_lines > RD_MAX_LINES) lay->max_lines = RD_MAX_LINES;
}

/* ---- position persistence ----
 * One line per book in RD_POS_FILE: "<offset>\t<path>". Rewritten whole (the
 * file is a few KB at most), with the current book moved to the front so the
 * list self-trims to the most recently read. */

static size_t load_saved_pos(const char *path)
{
    size_t found = 0;
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    File f = SD.open(RD_POS_FILE, FILE_READ);
    if (f) {
        char line[160];
        while (f.available()) {
            size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
            line[n] = '\0';
            char *tab = strchr(line, '\t');
            if (!tab) continue;
            *tab = '\0';
            if (!strcmp(tab + 1, path)) { found = (size_t)strtoul(line, NULL, 10); break; }
        }
        f.close();
    }
    shared_spi_unlock();
    return found;
}

static void save_pos(void)
{
    if (!g_path[0]) return;

    /* Read the existing entries (minus this book) so we can rewrite the file. */
    static char keep[24][160];
    int keep_n = 0;

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    if (!SD.exists(RD_START_DIR)) SD.mkdir(RD_START_DIR);
    File f = SD.open(RD_POS_FILE, FILE_READ);
    if (f) {
        char line[160];
        while (f.available() && keep_n < (int)(sizeof(keep) / sizeof(keep[0]))) {
            size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
            line[n] = '\0';
            if (n == 0) continue;
            const char *tab = strchr(line, '\t');
            if (!tab || !strcmp(tab + 1, g_path)) continue;   /* drop old entry */
            strncpy(keep[keep_n], line, sizeof(keep[0]) - 1);
            keep[keep_n][sizeof(keep[0]) - 1] = '\0';
            keep_n++;
        }
        f.close();
    }

    File o = SD.open(RD_POS_FILE, FILE_WRITE);
    if (o) {
        o.printf("%lu\t%s\n", (unsigned long)g_pos, g_path);
        for (int i = 0; i < keep_n; i++) o.printf("%s\n", keep[i]);
        o.close();
    }
    shared_spi_unlock();
}

/* ---- book loading ---- */

static void free_book(void)
{
    if (g_text) { free(g_text); g_text = NULL; }
    g_len = 0;
    g_stack_n = 0;
}

static bool load_book(const char *path)
{
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    File f = SD.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        shared_spi_unlock();
        if (status_label) lv_label_set_text(status_label, "Cannot open file");
        return false;
    }
    size_t len = f.size();
    if (len > RD_MAX_FILE) {
        f.close();
        shared_spi_unlock();
        if (status_label) lv_label_set_text(status_label, "File too large");
        return false;
    }
    char *buf = (char *)ps_malloc(len + 1);
    if (!buf) {
        f.close();
        shared_spi_unlock();
        if (status_label) lv_label_set_text(status_label, "Out of memory");
        return false;
    }
    size_t got = 0;
    while (got < len) {
        int n = f.read((uint8_t *)buf + got, len - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    f.close();
    shared_spi_unlock();

    buf[got] = '\0';

    free_book();
    g_text = buf;
    g_len = got;
    strncpy(g_path, path, sizeof(g_path) - 1);
    g_path[sizeof(g_path) - 1] = '\0';

    /* Skip a UTF-8 BOM so it doesn't render as a stray glyph. */
    size_t start = 0;
    if (g_len >= 3 && (uint8_t)g_text[0] == 0xEF && (uint8_t)g_text[1] == 0xBB &&
        (uint8_t)g_text[2] == 0xBF) start = 3;

    size_t saved = load_saved_pos(path);
    g_pos = (saved > start && saved < g_len) ? txt_utf8_align(g_text, saved) : start;
    g_stack_n = 0;

    Serial.printf("[READ] %s: %u bytes, resume at %u\n", path,
                  (unsigned)g_len, (unsigned)g_pos);
    return true;
}

/* ---- paging ---- */

static void push_page(size_t off)
{
    if (!g_stack) return;
    if (g_stack_n > 0 && g_stack[g_stack_n - 1] == off) return;
    if (g_stack_n >= RD_MAX_STACK) {
        /* Drop the oldest half rather than stop recording. */
        memmove(g_stack, g_stack + RD_MAX_STACK / 2,
                (RD_MAX_STACK / 2) * sizeof(size_t));
        g_stack_n = RD_MAX_STACK / 2;
    }
    g_stack[g_stack_n++] = off;
}

static void next_page(void)
{
    if (!g_text || g_pos >= g_len) return;
    txt_layout_t lay;
    make_layout(&lay);
    txt_line_t lines[RD_MAX_LINES];
    int n = 0;
    size_t next = txt_layout_page(g_text, g_len, g_pos, &lay, lines, &n);
    if (next <= g_pos || next > g_len) return;      /* already at the end */
    push_page(g_pos);
    g_pos = next;
    render_page();
}

static void prev_page(void)
{
    if (!g_text || g_pos == 0) return;

    /* Exact when we've been here before this session. */
    if (g_stack_n > 0) {
        g_pos = g_stack[--g_stack_n];
        render_page();
        return;
    }

    /* Otherwise (fresh resume) re-flow from a line boundary a little earlier and
     * take the last page that starts before the current position. Pagination
     * phase may differ from a run started at byte 0, but no text is skipped or
     * repeated, which is what matters. */
    size_t probe = (g_pos > RD_BACK_PROBE) ? g_pos - RD_BACK_PROBE : 0;
    if (probe > 0) {
        while (probe < g_pos && g_text[probe] != '\n') probe++;
        if (probe < g_pos) probe++;
        probe = txt_utf8_align(g_text, probe);
    }

    txt_layout_t lay;
    make_layout(&lay);
    txt_line_t lines[RD_MAX_LINES];
    size_t cur = probe, prev = probe;
    int guard = 0;
    while (cur < g_pos && guard++ < 4096) {
        int n = 0;
        size_t next = txt_layout_page(g_text, g_len, cur, &lay, lines, &n);
        if (next <= cur) break;
        if (next >= g_pos) { prev = cur; break; }
        prev = cur;
        cur = next;
    }
    g_pos = prev;
    render_page();
}

/* ---- rendering ---- */

static void render_page(void)
{
    if (!text_label) return;
    if (!g_text) {
        lv_label_set_text(text_label, "");
        if (status_label) lv_label_set_text(status_label, "No book open");
        return;
    }

    txt_layout_t lay;
    make_layout(&lay);
    txt_line_t lines[RD_MAX_LINES];
    int n = 0;
    txt_layout_page(g_text, g_len, g_pos, &lay, lines, &n);

    /* Join the pre-wrapped lines with newlines. Tabs are expanded here so the
     * label never sees one (its width was already counted during layout). */
    static char page_buf[4096];
    size_t o = 0;
    for (int i = 0; i < n && o < sizeof(page_buf) - 8; i++) {
        for (uint16_t k = 0; k < lines[i].len && o < sizeof(page_buf) - 8; k++) {
            char c = g_text[lines[i].off + k];
            if (c == '\t') {
                for (int t = 0; t < 4 && o < sizeof(page_buf) - 8; t++) page_buf[o++] = ' ';
            } else {
                page_buf[o++] = c;
            }
        }
        if (i < n - 1) page_buf[o++] = '\n';
    }
    page_buf[o] = '\0';

    lv_obj_set_style_text_font(text_label, cur_font(), LV_PART_MAIN);
    lv_label_set_text(text_label, page_buf);

    if (status_label) {
        const char *base = strrchr(g_path, '/');
        base = base ? base + 1 : g_path;
        int pct = g_len ? (int)((uint64_t)g_pos * 100 / g_len) : 0;
        lv_label_set_text_fmt(status_label, "%s  %d%%", base, pct);
    }
    ui_disp_full_refr();
}

/* ---- browser ---- */

static void browser_item_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < -1 || idx >= entry_count) return;

    if (idx == -1) {
        char up[RD_PATH_LEN];
        parent_dir_into(up, sizeof(up), cur_dir);
        strncpy(cur_dir, up, sizeof(cur_dir) - 1);
        cur_dir[sizeof(cur_dir) - 1] = '\0';
        refresh_browser();
        ui_disp_full_refr();
        return;
    }
    if (entries[idx].is_dir) {
        char sub[RD_PATH_LEN];
        path_join_into(sub, sizeof(sub), cur_dir, entries[idx].name);
        strncpy(cur_dir, sub, sizeof(cur_dir) - 1);
        cur_dir[sizeof(cur_dir) - 1] = '\0';
        refresh_browser();
        ui_disp_full_refr();
        return;
    }

    if (g_text) save_pos();                    /* remember the outgoing book */
    char path[RD_PATH_LEN];
    path_join_into(path, sizeof(path), cur_dir, entries[idx].name);
    if (load_book(path)) {
        show_rd_page(0);
        render_page();
    } else {
        ui_disp_full_refr();
    }
}

static void refresh_browser(void)
{
    entry_count = 0;

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    File dir = SD.open(cur_dir);
    if (dir && dir.isDirectory()) {
        for (int pass = 0; pass < 2; pass++) {      /* folders first, then files */
            dir.rewindDirectory();
            File e = dir.openNextFile();
            while (e && entry_count < RD_MAX_ENTRIES) {
                bool d = e.isDirectory();
                const char *n = e.name();
                const char *base = strrchr(n, '/');
                base = base ? base + 1 : n;
                if (base[0] != '.' &&
                    ((pass == 0 && d) || (pass == 1 && !d && is_text_name(base)))) {
                    strncpy(entries[entry_count].name, base, RD_NAME_LEN - 1);
                    entries[entry_count].name[RD_NAME_LEN - 1] = '\0';
                    entries[entry_count].is_dir = d;
                    entry_count++;
                }
                e.close();
                e = dir.openNextFile();
            }
        }
    }
    if (dir) dir.close();
    shared_spi_unlock();

    if (!browser_list) return;
    lv_obj_clean(browser_list);
    if (strcmp(cur_dir, "/") != 0) {
        lv_obj_t *b = lv_list_add_btn(browser_list, LV_SYMBOL_DIRECTORY, "..");
        lv_obj_add_event_cb(b, browser_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    }
    for (int i = 0; i < entry_count; i++) {
        lv_obj_t *b = lv_list_add_btn(browser_list,
                                      entries[i].is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE,
                                      entries[i].name);
        lv_obj_add_event_cb(b, browser_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (browser_path_lbl) lv_label_set_text_fmt(browser_path_lbl, "%s", cur_dir);
}

/* ---- pages ---- */

static const char *page_titles[] = {"Read", "Books"};

static void show_rd_page(int pg)
{
    rd_page = pg;
    for (int i = 0; i < RD_PAGE_COUNT; i++) {
        if (!pages[i]) continue;
        if (i == pg) lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (page_ind)
        lv_label_set_text_fmt(page_ind, "%s [%d/%d]", page_titles[pg], pg + 1, RD_PAGE_COUNT);
    if (pg == 1) refresh_browser();
}

/* ---- keyboard ---- */

void reader_keyboard_poll(void)
{
    if (!rd_kbd_active) return;
    char c;
    if (!keypad_get_val(&c)) return;
    keypad_set_flag();

    if (c == '\b') {
        if (rd_page == 1) { show_rd_page(0); ui_disp_full_refr(); }
        else { save_pos(); rd_kbd_active = false; scr_mgr_pop(false); }
        return;
    }
    if (c == '\n') {
        show_rd_page((rd_page + 1) % RD_PAGE_COUNT);
        ui_disp_full_refr();
        return;
    }
    if (rd_page != 0) return;

    switch (c) {
    case ' ':
    case 'm':
    case 's': next_page(); break;
    case 'n':
    case 'w': prev_page(); break;
    case 'i':                                   /* bigger text */
        if (!g_big_font) { g_big_font = true;  g_stack_n = 0; render_page(); }
        break;
    case 'o':                                   /* smaller text */
        if (g_big_font)  { g_big_font = false; g_stack_n = 0; render_page(); }
        break;
    default: break;
    }
}

/* ---- lifecycle ---- */

static void rd_back_cb(lv_event_t *e)
{
    save_pos();
    rd_kbd_active = false;
    scr_mgr_pop(false);
}

static lv_obj_t *make_page(lv_obj_t *parent)
{
    lv_obj_t *pg = lv_obj_create(parent);
    lv_obj_set_size(pg, 240, 292);
    lv_obj_align(pg, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_border_width(pg, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pg, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(pg, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
    return pg;
}

static void rd_create(lv_obj_t *parent)
{
    scr_back_btn_create(parent, "Reader", rd_back_cb);

    page_ind = lv_label_create(parent);
    lv_obj_set_style_text_font(page_ind, &Font_Mono_Bold_14, LV_PART_MAIN);
    lv_obj_align(page_ind, LV_ALIGN_BOTTOM_RIGHT, -4, -2);

    if (!g_stack) g_stack = (size_t *)ps_calloc(RD_MAX_STACK, sizeof(size_t));

    /* Page 0: text */
    pages[0] = make_page(parent);

    text_label = lv_label_create(pages[0]);
    lv_obj_set_width(text_label, RD_TEXT_W);
    /* Lines are pre-wrapped, so keep LVGL from re-wrapping (it cannot break CJK
     * anyway) and simply clip anything unexpected. */
    lv_label_set_long_mode(text_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(text_label, cur_font(), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(text_label, 2, LV_PART_MAIN);
    lv_obj_align(text_label, LV_ALIGN_TOP_LEFT, RD_TEXT_X, RD_TEXT_Y - 28);
    lv_label_set_text(text_label, "");

    status_label = lv_label_create(pages[0]);
    lv_obj_set_width(status_label, 160);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_LEFT, 4, 0);
    lv_obj_set_style_text_font(status_label, &Font_Mono_Bold_14, LV_PART_MAIN);
    lv_label_set_text(status_label, "space/m next  n prev  i/o size");

    /* Page 1: browser */
    pages[1] = make_page(parent);

    browser_path_lbl = lv_label_create(pages[1]);
    lv_obj_set_width(browser_path_lbl, 232);
    lv_label_set_long_mode(browser_path_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(browser_path_lbl, LV_ALIGN_TOP_LEFT, 4, 0);
    lv_obj_set_style_text_font(browser_path_lbl, &Font_Mono_Bold_14, LV_PART_MAIN);
    lv_label_set_text(browser_path_lbl, cur_dir);

    browser_list = lv_list_create(pages[1]);
    lv_obj_set_size(browser_list, 236, 250);
    lv_obj_align(browser_list, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_pad_all(browser_list, 2, LV_PART_MAIN);

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    if (!SD.exists(RD_START_DIR)) SD.mkdir(RD_START_DIR);
    bool have_dir = SD.exists(RD_START_DIR);
    shared_spi_unlock();
    if (!have_dir) strcpy(cur_dir, "/");

    /* Reopen the last book if it is still there, otherwise start in the browser. */
    if (g_text) { show_rd_page(0); render_page(); }
    else        { show_rd_page(1); }

    rd_kbd_active = true;
}

static void rd_entry(void) { rd_kbd_active = true; ui_disp_full_refr(); }

static void rd_exit(void)
{
    save_pos();
    rd_kbd_active = false;
    ui_disp_full_refr();
}

static void rd_destroy(void)
{
    save_pos();
    rd_kbd_active = false;
    free_book();
    if (g_stack) { free(g_stack); g_stack = NULL; }
    g_stack_n = 0;
    text_label = status_label = browser_list = browser_path_lbl = page_ind = NULL;
    for (int i = 0; i < RD_PAGE_COUNT; i++) pages[i] = NULL;
    entry_count = 0;
    g_path[0] = '\0';
}

extern "C" {
scr_lifecycle_t screen_reader = {
    .create  = rd_create,
    .entry   = rd_entry,
    .exit    = rd_exit,
    .destroy = rd_destroy,
};
}
