/**
 * @file      ui_image.cpp
 * @brief     Image viewer — PNG / JPEG / BMP on the e-ink panel.
 *
 * Page 1: the image, fitted to the panel (or shown 1:1 when it is smaller than
 *         the panel), with w/a/s/d panning, i/o zoom and n/m to step through
 *         the other images in the same folder.
 * Page 2: a file browser for picking an image / changing folder.
 *
 * Pipeline: decode to 8-bit grayscale in PSRAM once (img_codec.h), then for
 * every redraw resample that buffer into the visible region and Floyd-Steinberg
 * dither it to the 1-bpp canvas. Keeping the grayscale master around means zoom
 * and pan never re-decode, and dithering from grays rather than thresholding
 * keeps photos legible on a black-and-white panel.
 */
#include "Arduino.h"
#include "ui_deckpro.h"
#include "ui_deckpro_port.h"
#include "factory.h"
#include "img_codec.h"
#include "img_render.h"
#include "src/assets.h"
#include "utilities.h"
#include <SD.h>
#include <math.h>

extern void shared_spi_lock(void);
extern void shared_spi_unlock(void);
extern void shared_spi_prepare_device(int cs_pin);

#define IMG_PAGE_COUNT   2
#define VIEW_X           0
#define VIEW_Y           28
#define VIEW_W           240
#define VIEW_H           264

#define IMG_START_DIR    "/images"      /* where screenshots land */
#define IMG_MAX_ENTRIES  128
#define IMG_NAME_LEN     40
#define IMG_PATH_LEN     96

/* Decode budgets. JPEG descales to fit, so its cap only bounds the result;
 * PNG/BMP are rejected past theirs because they can't be decoded smaller. */
#define IMG_MAX_PIXELS_JPG   (2u * 1000u * 1000u)
#define IMG_MAX_PIXELS_PNG   (1500u * 1000u)
#define IMG_MAX_PIXELS_BMP   (4u * 1000u * 1000u)
#define IMG_MAX_FILE_BYTES   (6u * 1024u * 1024u)

#define ZOOM_MAX         8.0f
#define ZOOM_STEP        1.5f

/* ---- state ---- */

static lv_obj_t *pages[IMG_PAGE_COUNT] = {};
static lv_obj_t *page_ind = NULL;
static int img_page = 0;
static bool img_kbd_active = false;

/* Page 1 */
static lv_obj_t *view_canvas = NULL;
static lv_color_t *view_buf = NULL;
static lv_obj_t *view_status = NULL;

/* Page 2 */
static lv_obj_t *browser_list = NULL;
static lv_obj_t *browser_path_lbl = NULL;

/* Decoded image */
static uint8_t *g_gray = NULL;          /* w*h luma, PSRAM */
static int g_w = 0, g_h = 0;
static char g_path[IMG_PATH_LEN] = "";

/* View transform (see img_render.h) plus the zoom-out floor. */
static img_view_t g_view = {0, 0, 1.0f, 0, 0};
static float g_min_scale = 1.0f;

/* Directory contents. Directories sort first, then images. */
struct entry { char name[IMG_NAME_LEN]; bool is_dir; };
static entry entries[IMG_MAX_ENTRIES];
static int entry_count = 0;
static char cur_dir[IMG_PATH_LEN] = IMG_START_DIR;

/* Index of the currently shown image within `entries` (-1 = none). */
static int cur_img_idx = -1;

static void show_img_page(int pg);
static void render_view(void);
static void refresh_browser(void);

/* ---- path helpers ---- */

static const char *ext_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : "";
}

static bool ext_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static bool is_image_name(const char *name)
{
    const char *e = ext_of(name);
    return ext_eq(e, "png") || ext_eq(e, "jpg") || ext_eq(e, "jpeg") || ext_eq(e, "bmp");
}

static void path_join_into(char *dst, size_t cap, const char *dir, const char *leaf)
{
    if (!strcmp(dir, "/")) snprintf(dst, cap, "/%s", leaf);
    else                   snprintf(dst, cap, "%s/%s", dir, leaf);
}

static void parent_dir_into(char *dst, size_t cap, const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) { snprintf(dst, cap, "/"); return; }
    size_t n = (size_t)(slash - path);
    if (n >= cap) n = cap - 1;
    memcpy(dst, path, n);
    dst[n] = '\0';
}

/* ---- decode ---- */

static void free_image(void)
{
    if (g_gray) { free(g_gray); g_gray = NULL; }
    g_w = g_h = 0;
}

static const char *err_text(int e)
{
    switch (e) {
    case IMG_ERR_FORMAT:  return "bad or corrupt file";
    case IMG_ERR_TOO_BIG: return "image too large";
    case IMG_ERR_DECODE:  return "decode failed";
    case IMG_ERR_MEMORY:  return "out of memory";
    case IMG_ERR_UNSUPP:  return "unsupported variant";
    default:              return "error";
    }
}

/* Reset zoom/pan for a freshly loaded image: fit to the view, but never scale
 * a small image up — those show at their native size. */
static void reset_view(void)
{
    g_min_scale = img_view_fit_scale(g_w, g_h, VIEW_W, VIEW_H);
    g_view.gw = g_w;
    g_view.gh = g_h;
    g_view.scale = g_min_scale;
    g_view.off_x = g_view.off_y = 0;
}

static bool load_image(const char *path)
{
    /* Read the whole file under the SPI lock, then decode with the bus free. */
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    File f = SD.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        shared_spi_unlock();
        if (view_status) lv_label_set_text_fmt(view_status, "Cannot open %s", path);
        return false;
    }
    size_t len = f.size();
    if (len == 0 || len > IMG_MAX_FILE_BYTES) {
        f.close();
        shared_spi_unlock();
        if (view_status) lv_label_set_text(view_status, "File too large");
        return false;
    }
    uint8_t *buf = (uint8_t *)ps_malloc(len);
    if (!buf) {
        f.close();
        shared_spi_unlock();
        if (view_status) lv_label_set_text(view_status, "Out of memory");
        return false;
    }
    size_t got = f.read(buf, len);
    f.close();
    shared_spi_unlock();

    if (got != len) {
        free(buf);
        if (view_status) lv_label_set_text(view_status, "Read error");
        return false;
    }

    uint8_t *gray = NULL;
    int w = 0, h = 0;
    const char *e = ext_of(path);
    uint32_t t0 = millis();
    int rc;
    if (ext_eq(e, "png"))
        rc = img_png_decode_gray(buf, len, IMG_MAX_PIXELS_PNG, &gray, &w, &h);
    else if (ext_eq(e, "jpg") || ext_eq(e, "jpeg"))
        rc = img_jpg_decode_gray(buf, len, IMG_MAX_PIXELS_JPG, &gray, &w, &h);
    else if (ext_eq(e, "bmp"))
        rc = img_bmp_decode_gray(buf, len, IMG_MAX_PIXELS_BMP, &gray, &w, &h);
    else
        rc = IMG_ERR_UNSUPP;
    free(buf);

    if (rc != 0) {
        Serial.printf("[IMG] decode %s failed: %s (%d)\n", path, err_text(rc), rc);
        if (view_status) lv_label_set_text_fmt(view_status, "%s: %s",
                                               strrchr(path, '/') ? strrchr(path, '/') + 1 : path,
                                               err_text(rc));
        return false;
    }

    free_image();
    g_gray = gray;
    g_w = w;
    g_h = h;
    strncpy(g_path, path, sizeof(g_path) - 1);
    g_path[sizeof(g_path) - 1] = '\0';
    reset_view();

    Serial.printf("[IMG] %s: %dx%d decoded in %lu ms\n", path, w, h,
                  (unsigned long)(millis() - t0));
    return true;
}

/* ---- render ---- */

static void update_status(void)
{
    if (!view_status) return;
    if (!g_gray) { lv_label_set_text(view_status, "No image"); return; }
    const char *base = strrchr(g_path, '/');
    base = base ? base + 1 : g_path;
    lv_label_set_text_fmt(view_status, "%s  %dx%d  %d%%", base, g_w, g_h,
                          (int)lroundf(g_view.scale * 100.0f));
}

static void render_view(void)
{
    if (!view_canvas || !view_buf) return;

    img_view_clamp(&g_view, VIEW_W, VIEW_H);

    /* At LV_COLOR_DEPTH 1 a TRUE_COLOR canvas is one lv_color_t (one byte) per
     * pixel, so the renderer can fill the canvas buffer directly rather than
     * paying a call per pixel through lv_canvas_set_px. */
    img_view_render(g_gray, &g_view, (uint8_t *)view_buf, VIEW_W, VIEW_H,
                    lv_color_black().full, lv_color_white().full);

    lv_obj_invalidate(view_canvas);
    update_status();
}

/* ---- navigation ---- */

static void zoom_by(float factor)
{
    if (!g_gray) return;
    img_view_zoom(&g_view, factor, g_min_scale, ZOOM_MAX, VIEW_W, VIEW_H);
    render_view();
    ui_disp_full_refr();
}

static void pan_by(float fx, float fy)
{
    if (!g_gray) return;
    /* Step a fifth of a screenful, expressed in source pixels. */
    g_view.off_x += fx * ((float)VIEW_W / g_view.scale) * 0.2f;
    g_view.off_y += fy * ((float)VIEW_H / g_view.scale) * 0.2f;
    render_view();
    ui_disp_full_refr();
}

/* Step to the previous/next image within the current folder listing. */
static void step_image(int delta)
{
    if (entry_count == 0) return;

    int first = -1, last = -1;
    for (int i = 0; i < entry_count; i++) {
        if (entries[i].is_dir) continue;
        if (first < 0) first = i;
        last = i;
    }
    if (first < 0) return;

    int idx = cur_img_idx;
    if (idx < 0) idx = (delta > 0) ? first - 1 : last + 1;

    /* Walk over directory entries to land on the next actual image. */
    for (;;) {
        idx += delta;
        if (idx < 0 || idx >= entry_count) return;      /* stop at the ends */
        if (!entries[idx].is_dir) break;
    }

    char path[IMG_PATH_LEN];
    path_join_into(path, sizeof(path), cur_dir, entries[idx].name);
    if (load_image(path)) cur_img_idx = idx;
    render_view();
    ui_disp_full_refr();
}

/* ---- page 2: browser ---- */

static void browser_item_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < -1 || idx >= entry_count) return;

    if (idx == -1) {                                  /* ".." */
        char up[IMG_PATH_LEN];
        parent_dir_into(up, sizeof(up), cur_dir);
        strncpy(cur_dir, up, sizeof(cur_dir) - 1);
        cur_dir[sizeof(cur_dir) - 1] = '\0';
        cur_img_idx = -1;
        refresh_browser();
        ui_disp_full_refr();
        return;
    }

    if (entries[idx].is_dir) {
        char sub[IMG_PATH_LEN];
        path_join_into(sub, sizeof(sub), cur_dir, entries[idx].name);
        strncpy(cur_dir, sub, sizeof(cur_dir) - 1);
        cur_dir[sizeof(cur_dir) - 1] = '\0';
        cur_img_idx = -1;
        refresh_browser();
        ui_disp_full_refr();
        return;
    }

    char path[IMG_PATH_LEN];
    path_join_into(path, sizeof(path), cur_dir, entries[idx].name);
    if (load_image(path)) {
        cur_img_idx = (int)idx;
        show_img_page(0);
        render_view();
    }
    ui_disp_full_refr();
}

static void refresh_browser(void)
{
    entry_count = 0;

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    File dir = SD.open(cur_dir);
    if (dir && dir.isDirectory()) {
        /* Two passes so folders group above files without needing a sort. */
        for (int pass = 0; pass < 2; pass++) {
            dir.rewindDirectory();
            File e = dir.openNextFile();
            while (e && entry_count < IMG_MAX_ENTRIES) {
                bool d = e.isDirectory();
                const char *n = e.name();
                const char *base = strrchr(n, '/');
                base = base ? base + 1 : n;
                if ((pass == 0 && d) || (pass == 1 && !d && is_image_name(base))) {
                    strncpy(entries[entry_count].name, base, IMG_NAME_LEN - 1);
                    entries[entry_count].name[IMG_NAME_LEN - 1] = '\0';
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
                                      entries[i].is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_IMAGE,
                                      entries[i].name);
        lv_obj_add_event_cb(b, browser_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    if (browser_path_lbl) lv_label_set_text_fmt(browser_path_lbl, "%s", cur_dir);
}

/* ---- pages ---- */

static const char *page_titles[] = {"View", "Browse"};

static void show_img_page(int pg)
{
    img_page = pg;
    for (int i = 0; i < IMG_PAGE_COUNT; i++) {
        if (!pages[i]) continue;
        if (i == pg) lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (page_ind)
        lv_label_set_text_fmt(page_ind, "%s [%d/%d]", page_titles[pg], pg + 1, IMG_PAGE_COUNT);
    if (pg == 1) refresh_browser();
}

/* ---- keyboard ---- */

void image_keyboard_poll(void)
{
    if (!img_kbd_active) return;
    char c;
    if (!keypad_get_val(&c)) return;
    keypad_set_flag();

    if (c == '\b') {
        if (img_page == 1) { show_img_page(0); ui_disp_full_refr(); }
        else { img_kbd_active = false; scr_mgr_pop(false); }
        return;
    }
    if (c == '\n' || c == ' ') {
        show_img_page((img_page + 1) % IMG_PAGE_COUNT);
        ui_disp_full_refr();
        return;
    }
    if (img_page != 0) return;

    switch (c) {
    case 'a': pan_by(-1, 0); break;
    case 'd': pan_by(+1, 0); break;
    case 'w': pan_by(0, -1); break;
    case 's': pan_by(0, +1); break;
    case 'i': zoom_by(ZOOM_STEP); break;
    case 'o': zoom_by(1.0f / ZOOM_STEP); break;
    case 'n': step_image(-1); break;
    case 'm': step_image(+1); break;
    default: break;
    }
}

/* ---- lifecycle ---- */

static void img_back_cb(lv_event_t *e) { img_kbd_active = false; scr_mgr_pop(false); }

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

static void img_create(lv_obj_t *parent)
{
    scr_back_btn_create(parent, "Images", img_back_cb);

    page_ind = lv_label_create(parent);
    lv_obj_set_style_text_font(page_ind, &Font_Mono_Bold_14, LV_PART_MAIN);
    lv_obj_align(page_ind, LV_ALIGN_BOTTOM_RIGHT, -4, -2);

    /* Page 0: viewer */
    pages[0] = make_page(parent);

    view_buf = (lv_color_t *)ps_calloc((size_t)VIEW_W * VIEW_H, sizeof(lv_color_t));
    if (view_buf) {
        view_canvas = lv_canvas_create(pages[0]);
        lv_canvas_set_buffer(view_canvas, view_buf, VIEW_W, VIEW_H, LV_IMG_CF_TRUE_COLOR);
        lv_obj_align(view_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_canvas_fill_bg(view_canvas, lv_color_white(), LV_OPA_COVER);
    }

    view_status = lv_label_create(pages[0]);
    lv_obj_set_width(view_status, 170);
    lv_label_set_long_mode(view_status, LV_LABEL_LONG_DOT);
    lv_obj_align(view_status, LV_ALIGN_BOTTOM_LEFT, 2, 0);
    lv_obj_set_style_text_font(view_status, &Font_Mono_Bold_14, LV_PART_MAIN);
    lv_label_set_text(view_status, "wasd pan  i/o zoom  n/m prev/next");

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

    /* Start in /images when it exists, else at the card root. */
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    bool has_images = SD.exists(IMG_START_DIR);
    shared_spi_unlock();
    if (!has_images) strcpy(cur_dir, "/");

    show_img_page(1);           /* open on the browser: nothing is loaded yet */
    img_kbd_active = true;
}

static void img_entry(void)
{
    img_kbd_active = true;
    ui_disp_full_refr();
}

static void img_exit(void)
{
    img_kbd_active = false;
    ui_disp_full_refr();
}

static void img_destroy(void)
{
    img_kbd_active = false;
    free_image();
    view_canvas = view_status = browser_list = browser_path_lbl = page_ind = NULL;
    for (int i = 0; i < IMG_PAGE_COUNT; i++) pages[i] = NULL;
    if (view_buf) { free(view_buf); view_buf = NULL; }
    entry_count = 0;
    cur_img_idx = -1;
    g_path[0] = '\0';
}

extern "C" {
scr_lifecycle_t screen_image = {
    .create  = img_create,
    .entry   = img_entry,
    .exit    = img_exit,
    .destroy = img_destroy,
};
}
