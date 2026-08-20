/**
 * @file      ui_dictionary.cpp
 * @brief     Dictionary app with offline (SD StarDict) and online lookup.
 *
 * Page 1: search and results. Every enabled dictionary that has the word
 *         contributes an entry, headed by its bookname.
 * Page 2: which dictionaries to use. The choice is persisted, and it bounds
 *         memory as well as results — each loaded index lives in PSRAM and is
 *         freed when its dictionary is switched off.
 *
 * Indexes for the enabled dictionaries are loaded when the app opens rather
 * than on first use: the load is a multi-megabyte read from SD, and paying it
 * mid-search made the first lookup of each dictionary stall for seconds.
 */
#include "Arduino.h"
#include "ui_deckpro.h"
#include "ui_deckpro_port.h"
#include "power_mgr.h"
#include "cjk_font.h"
#include "dict_lookup.h"

#define DICT_PAGE_COUNT 2

static lv_obj_t *pages[DICT_PAGE_COUNT] = {};
static int dict_page = 0;

static lv_obj_t *search_ta = NULL;
static lv_obj_t *result_cont = NULL;   /* scrolls; holds result_label */
static lv_obj_t *result_label = NULL;
static lv_obj_t *status_label = NULL;
static lv_obj_t *dict_list = NULL;
static bool dict_kbd_active = false;

static void show_dict_page(int pg);
static void refresh_dict_list(void);

/* ---- search ---- */

static void update_status(void)
{
    if (!status_label) return;
    int total = dict_get_stardict_count();
    if (total > 0) {
        lv_label_set_text_fmt(status_label, "%d/%d dict(s) enabled",
                              dict_enabled_count(), total);
    } else if (dict_offline_en_available()) {
        lv_label_set_text(status_label, "Offline dict on SD");
    } else {
        lv_label_set_text(status_label, "Online only (WiFi)");
    }
}

static void do_search()
{
    const char *word = lv_textarea_get_text(search_ta);
    if (!word || word[0] == '\0') return;

    Serial.printf("[Dict] Searching: \"%s\"\n", word);
    lv_label_set_text(status_label, "Searching...");

    /* Record the query before looking it up, so the history reflects what was
     * asked for whether or not any dictionary had it. */
    dict_history_log(word);

    dict_result_t result;
    bool found = false;

    /* All enabled dictionaries, concatenated under their booknames. Lookups
     * are milliseconds once the indexes are resident, so this renders once
     * rather than progressively — an extra e-ink refresh costs far more than
     * the query it would hide. */
    if (dict_enabled_count() > 0) {
        found = dict_lookup_enabled(word, result);
    }

    if (!found && dict_offline_en_available()) {
        found = dict_lookup_offline_en(word, result);
    }

    if (!found) {
        const char *suggestions[MAX_SUGGESTIONS];
        int n = dict_prefix_search(word, suggestions, MAX_SUGGESTIONS, -1);
        if (n > 0) {
            static char buf[1024];
            int pos = snprintf(buf, sizeof(buf), "Did you mean:\n");
            for (int i = 0; i < n && pos < (int)sizeof(buf) - 1; i++)
                pos += snprintf(buf + pos, sizeof(buf) - pos, "  %s\n", suggestions[i]);
            lv_label_set_text(result_label, buf);
            if (result_cont) lv_obj_scroll_to_y(result_cont, 0, LV_ANIM_OFF);
            update_status();
            ui_disp_full_refr();
            return;
        }
    }

    if (!found) {
        found = dict_lookup_online(word, result);
    }

    if (found && result.found) {
        static char buf[4096];          /* several dictionaries can contribute */
        int n = snprintf(buf, sizeof(buf), "%s  %s\n%s",
                         result.phonetic.c_str(),
                         result.part_of_speech.c_str(),
                         result.definition.c_str());
        bool truncated = (n >= (int)sizeof(buf));
        Serial.printf("[Dict] result %d bytes%s -> label\n",
                      truncated ? (int)sizeof(buf) - 1 : n, truncated ? " (truncated)" : "");
        lv_label_set_text(result_label, buf);
        if (result_cont) lv_obj_scroll_to_y(result_cont, 0, LV_ANIM_OFF);
        update_status();
    } else {
        Serial.println("[Dict] no result to display");
        lv_label_set_text(result_label, "Word not found.");
        lv_label_set_text(status_label, "Try WiFi for online lookup.");
    }

    /* Ask for the repaint explicitly. Every other screen does this after
     * changing content; this one relied on LVGL's implicit invalidation, which
     * is no longer reliable here now that the refresh timer is throttled while
     * keys are arriving — and Enter is itself a keystroke. */
    ui_disp_full_refr();
}

/* ---- scrolling the result ----
 *
 * Definitions routinely run past one screen, more so now that several
 * dictionaries can contribute. Scroll a page at a time with LV_ANIM_OFF:
 * smooth scrolling would repaint the panel for every animation step, where
 * this costs exactly one refresh per press. Touch dragging works too. */

static void scroll_result(int dir)
{
    if (!result_cont) return;
    lv_coord_t h = lv_obj_get_height(result_cont);
    lv_coord_t step = (h > 60) ? (h - 24) : h;      /* keep a little overlap */
    lv_obj_scroll_by(result_cont, 0, dir > 0 ? -step : step, LV_ANIM_OFF);
    ui_disp_full_refr();
}

static void scroll_up_cb(lv_event_t *e)   { scroll_result(-1); }
static void scroll_down_cb(lv_event_t *e) { scroll_result(+1); }

/* ---- page 2: which dictionaries to use ---- */

static void open_dicts_cb(lv_event_t *e)
{
    show_dict_page(1);
    ui_disp_full_refr();
}

static void dict_toggle_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= dict_get_stardict_count()) return;

    bool now = !dict_is_enabled((int)idx);
    dict_set_enabled((int)idx, now);      /* frees the index when switched off */
    dict_save_selection();

    /* Bring the newly enabled dictionary's index in now, so the next search
     * doesn't stall on it. */
    if (now) dict_preload_enabled(NULL);

    refresh_dict_list();
    update_status();
    ui_disp_full_refr();
}

static void refresh_dict_list(void)
{
    if (!dict_list) return;
    lv_obj_clean(dict_list);

    int total = dict_get_stardict_count();
    if (total == 0) {
        lv_obj_t *b = lv_list_add_btn(dict_list, NULL, "No StarDict files in /stardict");
        lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
        return;
    }

    for (int i = 0; i < total; i++) {
        const char *name = dict_get_stardict_name(i);
        char label[80];
        snprintf(label, sizeof(label), "%s  %s",
                 dict_is_enabled(i) ? LV_SYMBOL_OK : " ", name ? name : "?");
        lv_obj_t *b = lv_list_add_btn(dict_list, NULL, label);
        lv_obj_set_style_text_font(b, &g_font_cn, LV_PART_MAIN);   /* names may be CJK */
        lv_obj_add_event_cb(b, dict_toggle_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

/* ---- pages ---- */

/* No page indicator: the Dicts button already says where the other page is,
 * and dropping it gives the whole bottom strip back to the result. */
static void show_dict_page(int pg)
{
    dict_page = pg;
    for (int i = 0; i < DICT_PAGE_COUNT; i++) {
        if (!pages[i]) continue;
        if (i == pg) lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (pg == 1) refresh_dict_list();
}

/* ---- keyboard ---- */

void dict_keyboard_poll()
{
    if (!dict_kbd_active || !search_ta) return;
    char c;
    if (!keypad_get_val(&c)) return;
    keypad_set_flag();

    if (dict_page != 0) {
        /* Any key returns to the search page rather than being swallowed
         * here — otherwise typing on this page looks like a dead keyboard. */
        show_dict_page(0);
        ui_disp_full_refr();
        if (c == '\n' || c == '\b') return;
        /* fall through so the keystroke still lands in the search box */
    }

    if (c == '\n') {
        do_search();                    /* Enter always searches */
    } else if (c == '\b') {
        const char *text = lv_textarea_get_text(search_ta);
        if (!text || text[0] == '\0') {
            dict_kbd_active = false;
            scr_mgr_pop(false);
        } else {
            lv_textarea_del_char(search_ta);
        }
    } else if (c >= ' ') {
        lv_textarea_add_char(search_ta, c);
    }
}

/* ---- lifecycle ---- */

static void dict_back_cb(lv_event_t *e)
{
    if (dict_page != 0) { show_dict_page(0); ui_disp_full_refr(); return; }
    dict_kbd_active = false;
    scr_mgr_pop(false);
}

static lv_obj_t *make_page(lv_obj_t *parent)
{
    lv_obj_t *pg = lv_obj_create(parent);
    lv_obj_set_size(pg, 236, 290);      /* y=28..318; nothing below it now */
    lv_obj_align(pg, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_border_width(pg, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pg, 2, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(pg, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
    return pg;
}

/* Shown while the indexes are read from SD, which takes a moment per book. */
static void preload_progress(const char *name, int n, int total)
{
    if (!status_label) return;
    lv_label_set_text_fmt(status_label, "Loading %s (%d/%d)...", name, n, total);
    lv_timer_handler();
}

static void dict_create(lv_obj_t *parent)
{
    scr_back_btn_create(parent, "Dictionary", dict_back_cb);

    /* ---- page 0: search ---- */
    pages[0] = make_page(parent);
    lv_obj_set_flex_flow(pages[0], LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pages[0], 4, LV_PART_MAIN);

    /* The label sizes itself to the full text; the container clips and scrolls
     * it. The scrollbar is left visible so it is obvious when there is more. */
    result_cont = lv_obj_create(pages[0]);
    lv_obj_set_width(result_cont, lv_pct(100));
    lv_obj_set_flex_grow(result_cont, 1);
    lv_obj_set_style_border_width(result_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(result_cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(result_cont, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(result_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(result_cont, LV_SCROLLBAR_MODE_ON);

    result_label = lv_label_create(result_cont);
    lv_obj_set_width(result_label, lv_pct(100));
    lv_obj_set_style_text_font(result_label, &g_font_cn, LV_PART_MAIN);
    lv_label_set_long_mode(result_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(result_label, "");

    status_label = lv_label_create(pages[0]);
    lv_obj_set_width(status_label, lv_pct(100));
    lv_obj_set_style_text_font(status_label, &g_font_cn, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_palette_main(LV_PALETTE_GREY), LV_PART_MAIN);

    /* Controls row. These are buttons rather than shortcuts because every
     * printable key belongs to the search box, and Enter has to mean search —
     * overloading it previously stranded you on a page that ignored typing. */
    lv_obj_t *row = lv_obj_create(pages[0]);
    lv_obj_set_size(row, lv_pct(100), 30);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    struct { const char *txt; lv_event_cb_t cb; lv_coord_t w; lv_coord_t x; } ctrls[] = {
        { LV_SYMBOL_LIST " Dicts", open_dicts_cb,  110,   0 },
        { LV_SYMBOL_UP,            scroll_up_cb,    50, 118 },
        { LV_SYMBOL_DOWN,          scroll_down_cb,  50, 174 },
    };
    for (unsigned i = 0; i < sizeof(ctrls) / sizeof(ctrls[0]); i++) {
        lv_obj_t *b = lv_btn_create(row);
        lv_obj_set_size(b, ctrls[i].w, 26);
        lv_obj_set_pos(b, ctrls[i].x, 0);
        lv_obj_set_style_radius(b, 6, LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_white(), LV_PART_MAIN);
        lv_obj_add_event_cb(b, ctrls[i].cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, ctrls[i].txt);
        lv_obj_set_style_text_color(l, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_center(l);
    }

    search_ta = lv_textarea_create(pages[0]);
    /* No cursor blink: each blink is a full e-ink refresh, so a focused
     * field would repaint the panel twice a second forever. */
    lv_obj_set_style_anim_time(search_ta, 0, LV_PART_CURSOR);
    lv_obj_set_width(search_ta, lv_pct(100));
    lv_obj_set_height(search_ta, 36);
    lv_textarea_set_placeholder_text(search_ta, "Type word, Enter to search");
    lv_textarea_set_one_line(search_ta, true);
    lv_textarea_set_max_length(search_ta, 64);
    lv_obj_set_style_text_font(search_ta, &g_font_cn, LV_PART_MAIN);

    /* ---- page 1: dictionary selection ---- */
    pages[1] = make_page(parent);
    dict_list = lv_list_create(pages[1]);
    lv_obj_set_size(dict_list, 232, 286);
    lv_obj_align(dict_list, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(dict_list, 2, LV_PART_MAIN);
    lv_obj_set_style_text_font(dict_list, &g_font_cn, LV_PART_MAIN);

    dict_scan_stardict();
    dict_load_selection();

    show_dict_page(0);
    update_status();

    /* Warm the indexes now so the first search is instant. */
    dict_preload_enabled(preload_progress);
    update_status();

    dict_kbd_active = true;
}

static void dict_entry(void) { power_acquire(PWR_WIFI); ui_disp_full_refr(); }
static void dict_exit(void) { power_release(PWR_WIFI); ui_disp_full_refr(); }
static void dict_destroy(void)
{
    dict_kbd_active = false;
    search_ta = result_label = result_cont = status_label = dict_list = NULL;
    for (int i = 0; i < DICT_PAGE_COUNT; i++) pages[i] = NULL;
}

scr_lifecycle_t screen_dictionary = {
    .create = dict_create,
    .entry = dict_entry,
    .exit = dict_exit,
    .destroy = dict_destroy,
};
