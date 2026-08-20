/**
 * @file      ui_notes.cpp
 * @brief     Notes with Markdown preview.
 *
 * Page 1: rendered preview — headings, lists, quotes, fenced code and rules
 *         each get their own font and indent. Wrapping goes through
 *         text_layout.c so Chinese notes break correctly (LVGL's own breaker
 *         cannot). Inline **bold** markers are stripped rather than styled:
 *         Montserrat ships in one weight, so there is no bold to render with.
 * Page 2: the note list — open, create or delete.
 * Edit:   an lv_textarea over the raw Markdown, same keypad plumbing the
 *         dictionary and voice-AI screens already use.
 *
 * Notes live in /notes as .md files.
 */
#include "Arduino.h"
#include "ui_deckpro.h"
#include "ui_deckpro_port.h"
#include "factory.h"
#include "text_layout.h"
#include "md_parse.h"
#include "md_layout.h"
#include "md_view.h"
#include "cjk_font.h"
#include "src/assets.h"
#include "utilities.h"
#include <SD.h>
#include <time.h>

extern void shared_spi_lock(void);
extern void shared_spi_unlock(void);
extern void shared_spi_prepare_device(int cs_pin);

#define NT_DIR          "/notes"
#define NT_MAX_NOTES    64
#define NT_NAME_LEN     48
#define NT_PATH_LEN     96
#define NT_MAX_TEXT     (32 * 1024)     /* editable note size cap */
#define NT_MAX_BLOCKS   1024
#define NT_VIEW_W       232
#define NT_VIEW_X       4
#define NT_VIEW_TOP     30
#define NT_VIEW_H       242
#define NT_MAX_ROWS     24
#define NT_LINE_GAP     2

/* Page indices */
#define NT_PAGE_VIEW    0
#define NT_PAGE_LIST    1
#define NT_PAGE_COUNT   2

static lv_obj_t *pages[NT_PAGE_COUNT] = {};
static lv_obj_t *page_ind = NULL;
static int nt_page = 0;
static bool nt_kbd_active = false;
static bool nt_editing = false;

/* Preview page */
static lv_obj_t *view_cont = NULL;
static lv_obj_t *view_status = NULL;
static lv_obj_t *rows[NT_MAX_ROWS] = {};

/* Edit overlay */
static lv_obj_t *edit_cont = NULL;
static lv_obj_t *edit_ta = NULL;

/* List page */
static lv_obj_t *note_list = NULL;

/* Open note */
static char  *g_text = NULL;
static size_t g_len = 0;
static char   g_path[NT_PATH_LEN] = "";
static bool   g_dirty = false;

static md_block_t *g_blocks = NULL;
static int g_nblocks = 0;

/* Pagination cursor, plus a stack of previous ones so paging back is exact. */
static md_cursor_t g_cur = {0, 0};
static md_cursor_t g_back[128];
static int g_back_n = 0;

static char note_names[NT_MAX_NOTES][NT_NAME_LEN];
static int note_count = 0;

static void show_nt_page(int pg);
static void refresh_list(void);
static void render_preview(void);

/* ---- preview rendering (styling lives in md_view.cpp) ---- */

static void nt_view(md_view_t *v)
{
    v->parent   = view_cont;
    v->rows     = rows;
    v->max_rows = NT_MAX_ROWS;
    v->view_w   = NT_VIEW_W;
    v->view_h   = NT_VIEW_H;
    v->origin_x = NT_VIEW_X;
    v->big      = false;
}

/* ---- note file handling ---- */

static void free_note(void)
{
    if (g_text)   { free(g_text);   g_text = NULL; }
    if (g_blocks) { free(g_blocks); g_blocks = NULL; }
    g_len = 0;
    g_nblocks = 0;
    g_cur.blk = g_cur.line = 0;
    g_back_n = 0;
}

static void reparse(void)
{
    if (!g_blocks) g_blocks = (md_block_t *)ps_calloc(NT_MAX_BLOCKS, sizeof(md_block_t));
    g_nblocks = g_blocks ? md_parse(g_text, g_len, g_blocks, NT_MAX_BLOCKS) : 0;
    g_cur.blk = g_cur.line = 0;
    g_back_n = 0;
}

static bool load_note(const char *path)
{
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    File f = SD.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        shared_spi_unlock();
        return false;
    }
    size_t len = f.size();
    if (len > NT_MAX_TEXT) len = NT_MAX_TEXT;
    char *buf = (char *)ps_malloc(NT_MAX_TEXT + 1);   /* room to keep editing */
    if (!buf) { f.close(); shared_spi_unlock(); return false; }
    size_t got = 0;
    while (got < len) {
        int n = f.read((uint8_t *)buf + got, len - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    f.close();
    shared_spi_unlock();
    buf[got] = '\0';

    free_note();
    g_text = buf;
    g_len = got;
    strncpy(g_path, path, sizeof(g_path) - 1);
    g_path[sizeof(g_path) - 1] = '\0';
    g_dirty = false;
    reparse();
    Serial.printf("[NOTE] opened %s (%u bytes, %d blocks)\n", path, (unsigned)g_len, g_nblocks);
    return true;
}

static void save_note(void)
{
    if (!g_text || !g_path[0] || !g_dirty) return;
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    if (!SD.exists(NT_DIR)) SD.mkdir(NT_DIR);
    SD.remove(g_path);                       /* truncate: FILE_WRITE appends */
    File f = SD.open(g_path, FILE_WRITE);
    if (f) {
        f.write((const uint8_t *)g_text, g_len);
        f.close();
        g_dirty = false;
        Serial.printf("[NOTE] saved %s (%u bytes)\n", g_path, (unsigned)g_len);
    } else {
        Serial.printf("[NOTE] save FAILED %s\n", g_path);
    }
    shared_spi_unlock();
}

static bool new_note(void)
{
    char path[NT_PATH_LEN];
    time_t now; time(&now);
    struct tm tm; localtime_r(&now, &tm);
    snprintf(path, sizeof(path), NT_DIR "/note_%04d%02d%02d_%02d%02d%02d.md",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    char *buf = (char *)ps_malloc(NT_MAX_TEXT + 1);
    if (!buf) return false;
    buf[0] = '\0';

    free_note();
    g_text = buf;
    g_len = 0;
    strncpy(g_path, path, sizeof(g_path) - 1);
    g_path[sizeof(g_path) - 1] = '\0';
    g_dirty = true;
    reparse();
    save_note();
    return true;
}

static void render_preview(void)
{
    if (!view_cont) return;

    md_view_t v;
    nt_view(&v);
    md_view_clear(&v);

    if (!g_text || g_nblocks == 0) {
        if (view_status)
            lv_label_set_text(view_status, g_text ? "(empty note)  e: edit" : "No note open");
        ui_disp_full_refr();
        return;
    }

    md_view_render(&v, g_text, g_blocks, g_nblocks, g_cur);

    if (view_status) {
        const char *base = strrchr(g_path, '/');
        base = base ? base + 1 : g_path;
        int pct = g_nblocks ? (g_cur.blk * 100 / g_nblocks) : 0;
        lv_label_set_text_fmt(view_status, "%s%s  %d%%  e:edit",
                              g_dirty ? "*" : "", base, pct);
    }
    ui_disp_full_refr();
}

static void preview_next(void)
{
    if (!g_text) return;
    md_view_t v;
    nt_view(&v);
    md_cursor_t nxt = md_view_measure(&v, g_text, g_blocks, g_nblocks, g_cur);
    if (nxt.blk >= g_nblocks) return;                       /* already at the end */
    if (nxt.blk == g_cur.blk && nxt.line == g_cur.line) return;
    if (g_back_n < (int)(sizeof(g_back) / sizeof(g_back[0]))) g_back[g_back_n++] = g_cur;
    g_cur = nxt;
    render_preview();
}

static void preview_prev(void)
{
    if (!g_text || g_back_n == 0) return;
    g_cur = g_back[--g_back_n];
    render_preview();
}

/* ---- edit mode ---- */

static void edit_apply(void)
{
    if (!edit_ta || !g_text) return;
    const char *t = lv_textarea_get_text(edit_ta);
    size_t n = strlen(t);
    if (n > NT_MAX_TEXT) n = NT_MAX_TEXT;
    memcpy(g_text, t, n);
    g_text[n] = '\0';
    g_len = n;
    g_dirty = true;
    reparse();
    save_note();
}

static void edit_done_cb(lv_event_t *e)
{
    edit_apply();
    nt_editing = false;
    if (edit_cont) lv_obj_add_flag(edit_cont, LV_OBJ_FLAG_HIDDEN);
    render_preview();
}

static void enter_edit(void)
{
    if (!g_text || !edit_cont || !edit_ta) return;
    nt_editing = true;
    lv_textarea_set_text(edit_ta, g_text);
    lv_obj_clear_flag(edit_cont, LV_OBJ_FLAG_HIDDEN);
    ui_disp_full_refr();
}

/* ---- note list ---- */

static void open_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= note_count) return;
    save_note();
    char path[NT_PATH_LEN];
    snprintf(path, sizeof(path), NT_DIR "/%s", note_names[idx]);
    if (load_note(path)) {
        show_nt_page(NT_PAGE_VIEW);
        render_preview();
    }
}

static void new_cb(lv_event_t *e)
{
    save_note();
    if (new_note()) {
        show_nt_page(NT_PAGE_VIEW);
        render_preview();
        enter_edit();
    }
}

static void delete_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= note_count) return;
    char path[NT_PATH_LEN];
    snprintf(path, sizeof(path), NT_DIR "/%s", note_names[idx]);
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    SD.remove(path);
    shared_spi_unlock();
    if (!strcmp(path, g_path)) { free_note(); g_path[0] = '\0'; }
    refresh_list();
    ui_disp_full_refr();
}

static void refresh_list(void)
{
    note_count = 0;
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    if (!SD.exists(NT_DIR)) SD.mkdir(NT_DIR);
    File dir = SD.open(NT_DIR);
    if (dir && dir.isDirectory()) {
        File e = dir.openNextFile();
        while (e && note_count < NT_MAX_NOTES) {
            if (!e.isDirectory()) {
                const char *n = e.name();
                const char *base = strrchr(n, '/');
                base = base ? base + 1 : n;
                if (base[0] != '.') {
                    strncpy(note_names[note_count], base, NT_NAME_LEN - 1);
                    note_names[note_count][NT_NAME_LEN - 1] = '\0';
                    note_count++;
                }
            }
            e.close();
            e = dir.openNextFile();
        }
        dir.close();
    }
    shared_spi_unlock();

    if (!note_list) return;
    lv_obj_clean(note_list);

    lv_obj_t *nb = lv_list_add_btn(note_list, LV_SYMBOL_PLUS, "New note");
        lv_obj_set_style_text_font(nb, &g_font_cn, LV_PART_MAIN);
    lv_obj_add_event_cb(nb, new_cb, LV_EVENT_CLICKED, NULL);

    for (int i = 0; i < note_count; i++) {
        lv_obj_t *b = lv_list_add_btn(note_list, LV_SYMBOL_FILE, note_names[i]);
        lv_obj_set_style_text_font(b, &g_font_cn, LV_PART_MAIN);
        lv_obj_add_event_cb(b, open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, delete_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    }
}

/* ---- pages ---- */

static const char *page_titles[] = {"Preview", "Notes"};

static void show_nt_page(int pg)
{
    nt_page = pg;
    for (int i = 0; i < NT_PAGE_COUNT; i++) {
        if (!pages[i]) continue;
        if (i == pg) lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (page_ind)
        lv_label_set_text_fmt(page_ind, "%s [%d/%d]", page_titles[pg], pg + 1, NT_PAGE_COUNT);
    if (pg == NT_PAGE_LIST) refresh_list();
}

/* ---- keyboard ---- */

void notes_keyboard_poll(void)
{
    if (!nt_kbd_active) return;
    char c;
    if (!keypad_get_val(&c)) return;
    keypad_set_flag();

    /* While editing, every key belongs to the text area. */
    if (nt_editing) {
        if (c == '\b')      lv_textarea_del_char(edit_ta);
        else if (c)         lv_textarea_add_char(edit_ta, c);
        return;
    }

    if (c == '\b') {
        if (nt_page == NT_PAGE_LIST) { show_nt_page(NT_PAGE_VIEW); ui_disp_full_refr(); }
        else { save_note(); nt_kbd_active = false; scr_mgr_pop(false); }
        return;
    }
    if (c == '\n') {
        show_nt_page((nt_page + 1) % NT_PAGE_COUNT);
        ui_disp_full_refr();
        return;
    }
    if (nt_page != NT_PAGE_VIEW) return;

    switch (c) {
    case ' ':
    case 'm':
    case 's': preview_next(); break;
    case 'n':
    case 'w': preview_prev(); break;
    case 'e': enter_edit();   break;
    default: break;
    }
}

/* ---- lifecycle ---- */

static void nt_back_cb(lv_event_t *e)
{
    if (nt_editing) { edit_done_cb(NULL); return; }
    save_note();
    nt_kbd_active = false;
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

static void nt_create(lv_obj_t *parent)
{
    scr_back_btn_create(parent, "Notes", nt_back_cb);

    page_ind = lv_label_create(parent);
    lv_obj_set_style_text_font(page_ind, &Font_Mono_Bold_14, LV_PART_MAIN);
    lv_obj_align(page_ind, LV_ALIGN_BOTTOM_RIGHT, -4, -2);

    /* Page 0: preview */
    pages[NT_PAGE_VIEW] = make_page(parent);

    view_cont = lv_obj_create(pages[NT_PAGE_VIEW]);
    lv_obj_set_size(view_cont, 240, NT_VIEW_H + 4);
    lv_obj_set_pos(view_cont, 0, 0);
    lv_obj_set_style_border_width(view_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(view_cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(view_cont, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(view_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(view_cont, LV_OBJ_FLAG_SCROLLABLE);

    view_status = lv_label_create(pages[NT_PAGE_VIEW]);
    lv_obj_set_width(view_status, 165);
    lv_label_set_long_mode(view_status, LV_LABEL_LONG_DOT);
    lv_obj_align(view_status, LV_ALIGN_BOTTOM_LEFT, 4, 0);
    lv_obj_set_style_text_font(view_status, &g_font_cn, LV_PART_MAIN);
    lv_label_set_text(view_status, "No note open");

    /* Page 1: note list */
    pages[NT_PAGE_LIST] = make_page(parent);
    note_list = lv_list_create(pages[NT_PAGE_LIST]);
    lv_obj_set_style_text_font(note_list, &g_font_cn, LV_PART_MAIN);
    lv_obj_set_size(note_list, 236, 270);
    lv_obj_align(note_list, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(note_list, 2, LV_PART_MAIN);

    /* Edit overlay, on top of everything and hidden until 'e'. */
    edit_cont = lv_obj_create(parent);
    lv_obj_set_size(edit_cont, 240, 292);
    lv_obj_align(edit_cont, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_border_width(edit_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(edit_cont, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(edit_cont, 2, LV_PART_MAIN);
    lv_obj_clear_flag(edit_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(edit_cont, LV_OBJ_FLAG_HIDDEN);

    edit_ta = lv_textarea_create(edit_cont);
    /* No cursor blink: each blink is a full e-ink refresh, so a focused
     * field would repaint the panel twice a second forever. */

    lv_obj_set_style_anim_time(edit_ta, 0, LV_PART_CURSOR);
    lv_obj_set_size(edit_ta, 232, 250);
    lv_obj_align(edit_ta, LV_ALIGN_TOP_MID, 0, 0);
    lv_textarea_set_one_line(edit_ta, false);
    lv_textarea_set_max_length(edit_ta, NT_MAX_TEXT);
    lv_obj_set_style_text_font(edit_ta, &g_font_cn, LV_PART_MAIN);
    lv_textarea_set_placeholder_text(edit_ta, "# Note\n\nType here...");

    lv_obj_t *done = lv_btn_create(edit_cont);
    lv_obj_set_size(done, 90, 30);
    lv_obj_align(done, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_radius(done, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(done, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(done, lv_color_white(), LV_PART_MAIN);
    lv_obj_add_event_cb(done, edit_done_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(done);
    lv_label_set_text(dl, LV_SYMBOL_OK " Done");
    lv_obj_set_style_text_color(dl, lv_color_black(), LV_PART_MAIN);
    lv_obj_center(dl);

    show_nt_page(g_text ? NT_PAGE_VIEW : NT_PAGE_LIST);
    if (g_text) render_preview();
    nt_kbd_active = true;
}

static void nt_entry(void) { nt_kbd_active = true; ui_disp_full_refr(); }

static void nt_exit(void)
{
    if (nt_editing) edit_apply();
    save_note();
    nt_kbd_active = false;
    ui_disp_full_refr();
}

static void nt_destroy(void)
{
    if (nt_editing) edit_apply();
    save_note();
    nt_kbd_active = false;
    nt_editing = false;
    free_note();
    { md_view_t v; nt_view(&v); md_view_clear(&v); }
    view_cont = view_status = note_list = page_ind = NULL;
    edit_cont = edit_ta = NULL;
    for (int i = 0; i < NT_PAGE_COUNT; i++) pages[i] = NULL;
    note_count = 0;
    g_path[0] = '\0';
}

extern "C" {
scr_lifecycle_t screen_notes = {
    .create  = nt_create,
    .entry   = nt_entry,
    .exit    = nt_exit,
    .destroy = nt_destroy,
};
}
