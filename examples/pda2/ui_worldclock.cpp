/**
 * @file      ui_worldclock.cpp
 * @brief     World clock — up to six cities at a glance, plus a map picker.
 *
 * Page 1: the selected cities and their local times.
 * Page 2: a world map with the current timezone band highlighted, the cities
 *         in that zone, and a search box with typeahead. Tapping a city adds
 *         or removes it from page 1.
 *
 * Times come from each city's POSIX TZ string (city_db.h) applied with
 * tzset(), so DST is handled without carrying the IANA database. Page 1 only
 * repaints when a displayed minute actually changes — on e-ink a redraw is
 * expensive, and a clock that repainted on a timer would never let the panel
 * (or the battery) rest.
 */
#include "Arduino.h"
#include "ui_deckpro.h"
#include "ui_deckpro_port.h"
#include "cjk_font.h"
#include "src/assets.h"
#include "world_map.h"
#include "city_db.h"
#include <Preferences.h>
#include <time.h>

#define WC_PAGE_COUNT   2
#define WC_MAX_SEL      6
#define WC_PAGE_W       236
#define WC_PAGE_H       290
#define MAP_W           232
#define MAP_H           104
#define WC_LIST_ROWS    8

static lv_obj_t *pages[WC_PAGE_COUNT] = {};
static int wc_page = 0;
static bool wc_kbd_active = false;

/* Page 1 */
static lv_obj_t *slot_lbl[WC_MAX_SEL] = {};
static lv_obj_t *hint_lbl = NULL;
static int last_shown_min = -1;

/* Page 2 */
static lv_obj_t *map_canvas = NULL;
static lv_color_t *map_buf = NULL;
static lv_obj_t *zone_lbl = NULL;
static lv_obj_t *city_list = NULL;
static lv_obj_t *search_ta = NULL;

/* Selection: indices into city_table. */
static int selected[WC_MAX_SEL];
static int sel_count = 0;

/* Timezone navigation */
static int16_t zones[64];
static int zone_count = 0;
static int zone_idx = 0;

static void show_wc_page(int pg);
static void refresh_city_list(void);
static void draw_map(void);
static void render_clocks(bool force);

/* ---- time ---- */

/* Local time in `c`. TZ is saved and restored so the device keeps its own
 * zone — every other screen reads the clock through localtime(). */
static void city_time(const city_t *c, struct tm *out)
{
    char saved[96] = "";
    const char *cur = getenv("TZ");
    if (cur) { strncpy(saved, cur, sizeof(saved) - 1); }

    setenv("TZ", city_tz(c), 1);
    tzset();
    time_t now;
    time(&now);
    localtime_r(&now, out);

    if (saved[0]) setenv("TZ", saved, 1);
    else          unsetenv("TZ");
    tzset();
}

/* Whole-day difference between a city and here, for the +1d / -1d marker. */
static int day_delta(const struct tm *there, const struct tm *here)
{
    if (there->tm_year != here->tm_year)
        return there->tm_year > here->tm_year ? 1 : -1;
    int d = there->tm_yday - here->tm_yday;
    return d > 0 ? 1 : (d < 0 ? -1 : 0);
}

/* ---- selection ---- */

static bool is_selected(int city_idx)
{
    for (int i = 0; i < sel_count; i++) if (selected[i] == city_idx) return true;
    return false;
}

static void save_selection(void)
{
    Preferences p;
    p.begin("wclock", false);
    p.putUChar("n", (uint8_t)sel_count);
    for (int i = 0; i < WC_MAX_SEL; i++) {
        char key[8];
        snprintf(key, sizeof(key), "c%d", i);
        /* Stored by name: the table can gain cities and shift indices. */
        p.putString(key, i < sel_count ? city_table[selected[i]].city : "");
    }
    p.end();
}

static void load_selection(void)
{
    Preferences p;
    p.begin("wclock", true);
    int n = p.getUChar("n", 0);
    if (n > WC_MAX_SEL) n = WC_MAX_SEL;
    sel_count = 0;
    for (int i = 0; i < n; i++) {
        char key[8];
        snprintf(key, sizeof(key), "c%d", i);
        String name = p.getString(key, "");
        int idx = city_index_of(name.c_str());
        if (idx >= 0) selected[sel_count++] = idx;
    }
    p.end();

    if (sel_count == 0) {
        /* Something recognisable on first run rather than an empty screen. */
        const char *defaults[] = { "London", "New York", "Tokyo" };
        for (unsigned i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
            int idx = city_index_of(defaults[i]);
            if (idx >= 0) selected[sel_count++] = idx;
        }
    }
}

static void toggle_city(int city_idx)
{
    for (int i = 0; i < sel_count; i++) {
        if (selected[i] != city_idx) continue;
        for (int j = i; j < sel_count - 1; j++) selected[j] = selected[j + 1];
        sel_count--;
        save_selection();
        return;
    }
    if (sel_count >= WC_MAX_SEL) return;      /* full; deselect something first */
    selected[sel_count++] = city_idx;
    save_selection();
}

/* ---- page 1: the clocks ---- */

static void render_clocks(bool force)
{
    time_t now;
    time(&now);
    struct tm here;
    localtime_r(&now, &here);

    /* Repaint only when the minute rolls over. */
    if (!force && here.tm_min == last_shown_min) return;
    last_shown_min = here.tm_min;

    for (int i = 0; i < WC_MAX_SEL; i++) {
        if (!slot_lbl[i]) continue;
        if (i >= sel_count) { lv_label_set_text(slot_lbl[i], ""); continue; }

        const city_t *c = &city_table[selected[i]];
        struct tm lt;
        city_time(c, &lt);

        int dd = day_delta(&lt, &here);
        char off[16];
        city_format_offset(city_std_offset_min(c), off, sizeof(off));

        lv_label_set_text_fmt(slot_lbl[i], "%02d:%02d %-14s %s%s",
                              lt.tm_hour, lt.tm_min, c->city,
                              dd > 0 ? "+1d " : (dd < 0 ? "-1d " : ""), off);
    }

    if (hint_lbl) {
        lv_label_set_text_fmt(hint_lbl, "%d/%d cities   %02d:%02d local",
                              sel_count, WC_MAX_SEL, here.tm_hour, here.tm_min);
    }
    ui_disp_full_refr();
}

/* ---- page 2: map ---- */

static inline int map_x(int lon) { return (lon + 180) * MAP_W / 360; }
static inline int map_y(int lat) { return (90 - lat) * MAP_H / 180; }

static void draw_map(void)
{
    if (!map_canvas) return;
    lv_canvas_fill_bg(map_canvas, lv_color_white(), LV_OPA_COVER);

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_black();
    line.width = 1;

    for (int i = 0; i < world_coastline_count; i++) {
        int x1 = map_x(world_coastline[i][1]), y1 = map_y(world_coastline[i][0]);
        int x2 = map_x(world_coastline[i][3]), y2 = map_y(world_coastline[i][2]);
        if (abs(x2 - x1) > MAP_W / 2) continue;         /* skip the wrap-around */
        lv_point_t p[2] = {{(lv_coord_t)x1, (lv_coord_t)y1},
                           {(lv_coord_t)x2, (lv_coord_t)y2}};
        lv_canvas_draw_line(map_canvas, p, 2, &line);
    }

    /* The nominal band for this offset: 15 degrees of longitude per hour,
     * centred on the meridian the offset corresponds to. Real zone borders
     * follow politics, so this is a pointer, not a boundary. */
    int off = zones[zone_idx];
    int centre = off * 15 / 60;
    int xl = map_x(centre - 7), xr = map_x(centre + 7);
    for (int y = 0; y < MAP_H; y += 3) {
        if (xl >= 0 && xl < MAP_W) lv_canvas_set_px(map_canvas, xl, y, lv_color_black());
        if (xr >= 0 && xr < MAP_W) lv_canvas_set_px(map_canvas, xr, y, lv_color_black());
    }

    /* Cities in this zone, filled for the ones already on page 1. */
    const city_t *inz[32];
    int n = city_in_zone(off, inz, 32);
    for (int i = 0; i < n; i++) {
        int x = map_x((int)city_lon(inz[i])), y = map_y((int)city_lat(inz[i]));
        bool sel = is_selected((int)(inz[i] - city_table));
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (!sel && dx && dy) continue;          /* hollow-ish if unselected */
                int px = x + dx, py = y + dy;
                if (px >= 0 && px < MAP_W && py >= 0 && py < MAP_H)
                    lv_canvas_set_px(map_canvas, px, py, lv_color_black());
            }
        }
    }
    lv_obj_invalidate(map_canvas);
}

/* ---- page 2: city list ---- */

static void city_item_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= city_count) return;
    toggle_city((int)idx);
    refresh_city_list();
    draw_map();
    render_clocks(true);           /* keep page 1 in step */
    ui_disp_full_refr();
}

static void refresh_city_list(void)
{
    if (!city_list) return;
    lv_obj_clean(city_list);

    const city_t *hits[32];
    int n;
    const char *q = search_ta ? lv_textarea_get_text(search_ta) : "";

    if (q && q[0]) {
        n = city_search_prefix(q, hits, 32);       /* typeahead */
    } else {
        n = city_in_zone(zones[zone_idx], hits, 32);
    }

    if (n == 0) {
        lv_obj_t *b = lv_list_add_btn(city_list, NULL, "No match");
        lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
        return;
    }

    for (int i = 0; i < n; i++) {
        int idx = (int)(hits[i] - city_table);
        char label[64];
        snprintf(label, sizeof(label), "%s %s, %s",
                 is_selected(idx) ? LV_SYMBOL_OK : " ", hits[i]->city, hits[i]->country);
        lv_obj_t *b = lv_list_add_btn(city_list, NULL, label);
        lv_obj_set_style_text_font(b, &g_font_cn, LV_PART_MAIN);
        lv_obj_add_event_cb(b, city_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    }
}

static void update_zone_label(void)
{
    if (!zone_lbl) return;
    char off[16];
    city_format_offset(zones[zone_idx], off, sizeof(off));
    const city_t *inz[32];
    int n = city_in_zone(zones[zone_idx], inz, 32);
    lv_label_set_text_fmt(zone_lbl, "%s  (%d)", off, n);
}

static void zone_step(int dir)
{
    if (zone_count == 0) return;
    zone_idx = (zone_idx + dir + zone_count) % zone_count;
    if (search_ta) lv_textarea_set_text(search_ta, "");   /* zone view, not search */
    update_zone_label();
    refresh_city_list();
    draw_map();
    ui_disp_full_refr();
}

static void zone_prev_cb(lv_event_t *e) { zone_step(-1); }
static void zone_next_cb(lv_event_t *e) { zone_step(+1); }

/* ---- pages ---- */

static void show_wc_page(int pg)
{
    wc_page = pg;
    for (int i = 0; i < WC_PAGE_COUNT; i++) {
        if (!pages[i]) continue;
        if (i == pg) lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (pg == 0) render_clocks(true);
    else         { update_zone_label(); refresh_city_list(); draw_map(); }
}

/* ---- keyboard ---- */

void worldclock_keyboard_poll(void)
{
    if (!wc_kbd_active) return;
    char c;
    if (!keypad_get_val(&c)) return;
    keypad_set_flag();

    if (wc_page == 0) {
        if (c == '\n')      { show_wc_page(1); ui_disp_full_refr(); }
        else if (c == '\b') { wc_kbd_active = false; scr_mgr_pop(false); }
        return;
    }

    /* Page 2: typing drives the search. */
    if (c == '\n') {
        show_wc_page(0);
        ui_disp_full_refr();
    } else if (c == '\b') {
        const char *t = lv_textarea_get_text(search_ta);
        if (!t || t[0] == '\0') {
            show_wc_page(0);
            ui_disp_full_refr();
        } else {
            lv_textarea_del_char(search_ta);
            refresh_city_list();
            ui_disp_full_refr();
        }
    } else if (c >= ' ') {
        lv_textarea_add_char(search_ta, c);
        refresh_city_list();       /* typeahead */
        ui_disp_full_refr();
    }
}

/* ---- lifecycle ---- */

static void wc_back_cb(lv_event_t *e)
{
    if (wc_page != 0) { show_wc_page(0); ui_disp_full_refr(); return; }
    wc_kbd_active = false;
    scr_mgr_pop(false);
}

static lv_obj_t *make_page(lv_obj_t *parent)
{
    lv_obj_t *pg = lv_obj_create(parent);
    lv_obj_set_size(pg, WC_PAGE_W, WC_PAGE_H);
    lv_obj_align(pg, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_border_width(pg, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pg, 2, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(pg, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
    return pg;
}

static void wc_create(lv_obj_t *parent)
{
    scr_back_btn_create(parent, "World Clock", wc_back_cb);

    zone_count = city_zone_offsets(zones, (int)(sizeof(zones) / sizeof(zones[0])));
    load_selection();

    /* Start on the zone of the first selected city, so the map opens somewhere
     * meaningful rather than at UTC-11. */
    if (sel_count > 0) {
        for (int i = 0; i < zone_count; i++) {
            if (zones[i] == city_std_offset_min(&city_table[selected[0]])) { zone_idx = i; break; }
        }
    }

    /* ---- page 0: clocks ---- */
    pages[0] = make_page(parent);
    for (int i = 0; i < WC_MAX_SEL; i++) {
        slot_lbl[i] = lv_label_create(pages[0]);
        lv_obj_set_width(slot_lbl[i], WC_PAGE_W - 8);
        lv_label_set_long_mode(slot_lbl[i], LV_LABEL_LONG_DOT);
        lv_obj_set_pos(slot_lbl[i], 2, 6 + i * 34);
        lv_obj_set_style_text_font(slot_lbl[i], &Font_Mono_Bold_16, LV_PART_MAIN);
        lv_label_set_text(slot_lbl[i], "");
    }
    hint_lbl = lv_label_create(pages[0]);
    lv_obj_set_width(hint_lbl, WC_PAGE_W - 8);
    lv_obj_align(hint_lbl, LV_ALIGN_BOTTOM_LEFT, 2, -2);
    lv_obj_set_style_text_font(hint_lbl, &Font_Mono_Bold_14, LV_PART_MAIN);
    lv_label_set_text(hint_lbl, "");

    /* ---- page 1: map + picker ---- */
    pages[1] = make_page(parent);

    map_buf = (lv_color_t *)ps_calloc((size_t)MAP_W * MAP_H, sizeof(lv_color_t));
    if (map_buf) {
        map_canvas = lv_canvas_create(pages[1]);
        lv_canvas_set_buffer(map_canvas, map_buf, MAP_W, MAP_H, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_pos(map_canvas, 0, 0);
    }

    /* zone row: prev / label / next */
    lv_obj_t *row = lv_obj_create(pages[1]);
    lv_obj_set_size(row, WC_PAGE_W - 4, 28);
    lv_obj_set_pos(row, 0, MAP_H + 2);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    struct { const char *txt; lv_event_cb_t cb; lv_coord_t x; } navs[] = {
        { LV_SYMBOL_LEFT,  zone_prev_cb,   0 },
        { LV_SYMBOL_RIGHT, zone_next_cb, 182 },
    };
    for (unsigned i = 0; i < sizeof(navs) / sizeof(navs[0]); i++) {
        lv_obj_t *b = lv_btn_create(row);
        lv_obj_set_size(b, 46, 26);
        lv_obj_set_pos(b, navs[i].x, 0);
        lv_obj_set_style_radius(b, 6, LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_white(), LV_PART_MAIN);
        lv_obj_add_event_cb(b, navs[i].cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, navs[i].txt);
        lv_obj_set_style_text_color(l, lv_color_black(), LV_PART_MAIN);
        lv_obj_center(l);
    }
    zone_lbl = lv_label_create(row);
    lv_obj_align(zone_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(zone_lbl, &Font_Mono_Bold_16, LV_PART_MAIN);

    city_list = lv_list_create(pages[1]);
    lv_obj_set_size(city_list, WC_PAGE_W - 4, WC_PAGE_H - MAP_H - 32 - 40);
    lv_obj_set_pos(city_list, 0, MAP_H + 32);
    lv_obj_set_style_pad_all(city_list, 2, LV_PART_MAIN);
    lv_obj_set_style_text_font(city_list, &g_font_cn, LV_PART_MAIN);

    search_ta = lv_textarea_create(pages[1]);
    lv_obj_set_style_anim_time(search_ta, 0, LV_PART_CURSOR);   /* no blink on e-ink */
    lv_obj_set_size(search_ta, WC_PAGE_W - 4, 34);
    lv_obj_align(search_ta, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_textarea_set_one_line(search_ta, true);
    lv_textarea_set_max_length(search_ta, 24);
    lv_textarea_set_placeholder_text(search_ta, "Search city...");
    lv_obj_set_style_text_font(search_ta, &lv_font_montserrat_14, LV_PART_MAIN);

    show_wc_page(0);
    wc_kbd_active = true;
}

static void wc_entry(void)
{
    wc_kbd_active = true;
    last_shown_min = -1;          /* always repaint on entry */
    render_clocks(true);
    ui_disp_full_refr();
}

static void wc_exit(void) { wc_kbd_active = false; ui_disp_full_refr(); }

static void wc_destroy(void)
{
    wc_kbd_active = false;
    for (int i = 0; i < WC_MAX_SEL; i++) slot_lbl[i] = NULL;
    hint_lbl = zone_lbl = city_list = search_ta = map_canvas = NULL;
    for (int i = 0; i < WC_PAGE_COUNT; i++) pages[i] = NULL;
    if (map_buf) { free(map_buf); map_buf = NULL; }
}

/* Called from loop(): keeps the displayed minute current without a timer that
 * would repaint the panel needlessly. */
void worldclock_tick(void)
{
    if (!wc_kbd_active || wc_page != 0) return;
    render_clocks(false);
}

extern "C" {
scr_lifecycle_t screen_worldclock = {
    .create  = wc_create,
    .entry   = wc_entry,
    .exit    = wc_exit,
    .destroy = wc_destroy,
};
}
