/**
 * @file      ui_dictionary.cpp
 * @brief     Dictionary app with offline (SD StarDict) and online lookup.
 *
 * Page 1: search and results. Every enabled dictionary that has the word
 *         contributes an entry, headed by its bookname.
 * Page 2: which dictionaries to use, reached by the gear beside the search
 *         box. The choice is persisted, and it bounds
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
 * dictionaries can contribute. Tap the top or bottom half of the result area
 * to move a page; dragging scrolls freely as it always did.
 *
 * The two don't collide: LVGL only sends LV_EVENT_CLICKED when the press
 * ended without scrolling, so a drag scrolls and a tap pages. */

static void scroll_result(int dir)
{
    if (!result_cont) return;

    /* The label's height is only known once laid out, and a search may have
     * replaced the text since the last refresh. */
    lv_obj_update_layout(result_cont);

    /* Clamp to what is actually left, so paging at either end doesn't burn a
     * refresh scrolling into blank space. */
    lv_coord_t room = (dir > 0) ? lv_obj_get_scroll_bottom(result_cont)
                                : lv_obj_get_scroll_top(result_cont);
    if (room <= 0) return;

    lv_coord_t h = lv_obj_get_height(result_cont);
    lv_coord_t step = (h > 60) ? (h - 24) : h;      /* keep a little overlap */
    if (step > room) step = room;

    lv_obj_scroll_by(result_cont, 0, dir > 0 ? -step : step, LV_ANIM_OFF);
    ui_disp_full_refr();
}

static void result_click_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_area_t a;
    lv_obj_get_coords(result_cont, &a);
    scroll_result(p.y > (a.y1 + a.y2) / 2 ? +1 : -1);
}

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

/* No page indicator: the gear beside the search box is where the other page
 * lives, and dropping the strip gives the space back to the result. */
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
    lv_obj_add_flag(result_cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(result_cont, result_click_cb, LV_EVENT_CLICKED, NULL);

    result_label = lv_label_create(result_cont);
    lv_obj_set_width(result_label, lv_pct(100));
    lv_obj_set_style_text_font(result_label, &g_font_cn, LV_PART_MAIN);
    lv_label_set_long_mode(result_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(result_label, "");

    status_label = lv_label_create(pages[0]);
    lv_obj_set_width(status_label, lv_pct(100));
    lv_obj_set_style_text_font(status_label, &g_font_cn, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_palette_main(LV_PALETTE_GREY), LV_PART_MAIN);

    /* Search row: the box takes the width it can, the dictionary chooser sits
     * beside it as an icon. It used to be a labelled button on a row of its
     * own, together with the up/down buttons that swiping has now replaced —
     * a whole 30 px strip spent on three controls, two of which are gone. */
    lv_obj_t *srow = lv_obj_create(pages[0]);
    lv_obj_set_width(srow, lv_pct(100));
    lv_obj_set_height(srow, 38);
    lv_obj_set_style_border_width(srow, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(srow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(srow, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(srow, 4, LV_PART_MAIN);
    lv_obj_clear_flag(srow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(srow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(srow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    search_ta = lv_textarea_create(srow);
    /* No cursor blink: each blink is a full e-ink refresh, so a focused
     * field would repaint the panel twice a second forever. */
    lv_obj_set_style_anim_time(search_ta, 0, LV_PART_CURSOR);
    lv_obj_set_flex_grow(search_ta, 1);
    lv_obj_set_height(search_ta, 36);
    lv_textarea_set_placeholder_text(search_ta, "Type word, Enter to search");
    lv_textarea_set_one_line(search_ta, true);
    lv_textarea_set_max_length(search_ta, 64);
    lv_obj_set_style_text_font(search_ta, &g_font_cn, LV_PART_MAIN);

    lv_obj_t *dbtn = lv_btn_create(srow);
    lv_obj_set_size(dbtn, 36, 36);
    lv_obj_set_style_radius(dbtn, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(dbtn, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dbtn, lv_color_white(), LV_PART_MAIN);
    lv_obj_add_event_cb(dbtn, open_dicts_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dlbl = lv_label_create(dbtn);
    lv_label_set_text(dlbl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(dlbl, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(dlbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(dlbl);

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
