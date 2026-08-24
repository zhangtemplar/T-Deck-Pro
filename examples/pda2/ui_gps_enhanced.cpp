/**
 * @file      ui_gps_enhanced.cpp
 * @brief     Enhanced GPS: overview, world map, tracker.
 *            Page 1: Status + coordinates + time
 *            Page 2: World map with position dot
 *            Page 3: Track recorder with trajectory visualization
 */
#include "Arduino.h"
#include "ui_deckpro.h"
#include "ui_deckpro_port.h"
#include "power_mgr.h"
#include "ui_gps_enhanced.h"
#include "garmin_img.h"
#include "map_draw.h"
#include "map_store.h"
#include "world_map.h"
#include "utilities.h"          /* BOARD_SD_CS */
#include <SD.h>
#include <vector>
#include <math.h>
#include <time.h>

extern void shared_spi_lock(void);
extern void shared_spi_unlock(void);
extern void shared_spi_prepare_device(int cs_pin);

#define GPX_DIR "/gpx"
#define GPX_MAX_FILES 64
#define GPS_PAGE_COUNT 4
#define MAP_X 5
#define MAP_Y 4
#define MAP_W 220
#define MAP_H 236
#define MAP_STATUS_H 16
#define WORLD_H 120   /* the built-in coastline keeps its own aspect */
#define TRACK_MAX 1000
#define TRACK_VIEW_X 5
#define TRACK_VIEW_Y 30
#define TRACK_VIEW_W 220
#define TRACK_VIEW_H 180

static lv_obj_t *pages[GPS_PAGE_COUNT] = {};
static lv_obj_t *page_ind = NULL;
static lv_timer_t *gps_timer = NULL;
static int gps_page = 0;
static bool gps_kbd_active = false;

/* Current GPS data */
static double cur_lat = 0, cur_lng = 0, cur_speed = 0;
static uint32_t cur_sats = 0;
static uint16_t cur_year = 0;
static uint8_t cur_month = 0, cur_day = 0, cur_hour = 0, cur_min = 0, cur_sec = 0;
static bool has_fix = false;

/* Page 1 widgets */
static lv_obj_t *lbl_overview = NULL;

/* Page 2: map canvas. Draws a Garmin .img from the card when there is one,
 * and falls back to the built-in world coastline when there is not — the
 * coarse view is still worth having with no card in. */
static lv_obj_t *map_canvas = NULL;
static lv_color_t *map_buf = NULL;

#define MAP_DIR "/maps"
static bool     vmap_ready   = false;   /* a .img is open           */
static bool     vmap_tried   = false;   /* don't rescan on every draw */
static char     vmap_name[40] = "";
static int32_t  view_lat = 0, view_lon = 0;   /* centre, degrees x 1e7 */
static int32_t  view_span = 2000000;          /* canvas height, 0.2 deg */
static bool     view_locked = false;          /* follow the fix        */
static int      view_level = 0;
static int      view_feats = 0;

/* 0.1 degree of latitude, about 11 km down the canvas. */
#define VIEW_SPAN_DEFAULT 1000000
/* More tiles than a redraw can pay for; see gimg_overlap_tiles(). */
#define VIEW_MAX_TILES    8

/* Map picker overlay, on the map page. A button rather than a key: w/a/s/d,
 * i/o and g already belong to panning. */
static lv_obj_t *pick_ovl = NULL;
static lv_obj_t *pick_list = NULL;
static lv_obj_t *pick_status = NULL;
static lv_obj_t *map_btn = NULL;
static lv_obj_t *map_btn_lbl = NULL;
static lv_obj_t *map_ctr_lbl = NULL;
static bool      pick_open = false;
static mapstore_entry_t pick_items[MAPSTORE_MAX];
static int       pick_count = 0;

/* Page 3: tracker */
struct track_pt { double lat, lng; uint32_t ms; };
static std::vector<track_pt> track;
static bool tracking = false;
static uint32_t track_start_ms = 0;
static uint32_t track_last_pt_ms = 0;
static float track_dist_m = 0;
static time_t track_base_epoch = 0;   /* UTC epoch at track_start_ms (0 = unknown) */
static bool track_saved = false;      /* GPX already written for the current track */
static char last_gpx_path[64] = "";
static lv_obj_t *lbl_track_info = NULL;
static lv_obj_t *track_canvas = NULL;
static lv_color_t *track_buf = NULL;

/* Page 4: saved-track browser */
static lv_obj_t *gpx_list = NULL;
static lv_obj_t *gpx_info = NULL;
static lv_obj_t *gpx_del_btn = NULL;
static char gpx_files[GPX_MAX_FILES][40];
static int gpx_count = 0;
static int gpx_selected = -1;

static const char *page_titles[] = {"Overview", "Map", "Tracker", "Tracks"};

static void refresh_gpx_list();

/* ---- Helpers ---- */

static float haversine_m(double lat1, double lon1, double lat2, double lon2)
{
    double dlat = (lat2 - lat1) * M_PI / 180.0;
    double dlon = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dlat/2)*sin(dlat/2) + cos(lat1*M_PI/180)*cos(lat2*M_PI/180)*sin(dlon/2)*sin(dlon/2);
    return 6371000.0 * 2.0 * atan2(sqrt(a), sqrt(1-a));
}

static void pick_close(void);

static void show_gps_page(int pg)
{
    if (pick_open && pg != 1) pick_close();
    /* The map page hands its bottom strip to the map chooser. */
    if (map_btn) {
        if (pg == 1) lv_obj_clear_flag(map_btn, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(map_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (page_ind) {
        if (pg == 1) lv_obj_add_flag(page_ind, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_clear_flag(page_ind, LV_OBJ_FLAG_HIDDEN);
    }
    gps_page = pg;
    for (int i = 0; i < GPS_PAGE_COUNT; i++) {
        if (pages[i]) {
            if (i == pg) lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (page_ind)
        lv_label_set_text_fmt(page_ind, "%s [%d/%d]", page_titles[pg], pg+1, GPS_PAGE_COUNT);

    if (pg == 3) refresh_gpx_list();
}

/* ---- Page 1: Overview ---- */

static void update_overview()
{
    if (!lbl_overview) return;
    if (has_fix) {
        lv_label_set_text_fmt(lbl_overview,
            "Status: Fix (%lu sats)\n\n"
            "Lat: %.6f\n"
            "Lon: %.6f\n"
            "Speed: %.1f km/h\n\n"
            "Date: %04d-%02d-%02d\n"
            "Time: %02d:%02d:%02d UTC",
            cur_sats, cur_lat, cur_lng, cur_speed,
            cur_year, cur_month, cur_day,
            cur_hour, cur_min, cur_sec);
    } else {
        /* Show how many bytes have come back from the receiver. It separates
         * the two very different failures that both look like "no fix":
         * a dead UART link (stays 0) versus a genuine cold search (climbs). */
        uint32_t chars = gps_chars_processed();
        lv_label_set_text_fmt(lbl_overview,
            "Status: No Fix\n"
            "Satellites: %lu\n"
            "Receiver: %s (%lu bytes)\n\n"
            "%s\n\n"
            "Date: %04d-%02d-%02d\n"
            "Time: %02d:%02d:%02d UTC",
            cur_sats,
            chars > 0 ? "talking" : "SILENT", (unsigned long)chars,
            chars > 0 ? "Searching...\nPlace outdoors with\nclear sky view."
                      : "No data from GPS.\nLink or power problem,\nnot a cold start.",
            cur_year, cur_month, cur_day,
            cur_hour, cur_min, cur_sec);
    }
}

/* ---- Page 2: World Map ---- */

/* Coastline lives in world_map.c, shared with the world-clock screen. */
#define COAST_SEGMENTS world_coastline_count
#define coastline      world_coastline


/* ---- vector map from a Garmin .img ---- */

static void pick_refresh(void);
static void map_btn_update(void);
static void draw_vector_map(void);
static bool vmap_key(char c);
static void map_ctrl_cb(lv_event_t *e);

/* Unpacking a zip blocks for a while; keep the panel saying something. */
static void unpack_progress(const char *what, uint32_t done, uint32_t total)
{
    if (!pick_status) return;
    if (total) lv_label_set_text_fmt(pick_status, "%s %u%%", what,
                                     (unsigned)((uint64_t)done * 100 / total));
    else       lv_label_set_text_fmt(pick_status, "%s...", what);
    lv_timer_handler();
}

/* Open `name` from /maps, unpacking it first if it is a zip. */
static bool vmap_open_named(const char *name)
{
    if (vmap_ready) { gimg_close(); vmap_ready = false; }
    vmap_name[0] = '\0';
    if (!name || !name[0]) return false;

    char path[128], err[96];
    if (!mapstore_resolve(name, path, sizeof(path), err, sizeof(err), unpack_progress)) {
        Serial.printf("[MAP] %s: %s\n", name, err);
        if (pick_status) lv_label_set_text(pick_status, err);
        return false;
    }

    if (pick_status) { lv_label_set_text(pick_status, "Opening..."); lv_timer_handler(); }

    uint32_t t0 = millis();
    vmap_ready = gimg_open(path);
    Serial.printf("[MAP] open %s: %s in %lu ms\n", path,
                  vmap_ready ? "ok" : "FAILED", (unsigned long)(millis() - t0));

    if (vmap_ready) {
        snprintf(vmap_name, sizeof(vmap_name), "%s", name);
        int32_t a, b, c, d;
        gimg_bounds(&a, &b, &c, &d);
        view_lat = (a + c) / 2;
        view_lon = (b + d) / 2;
        /* Open at a walkable zoom, not the whole map. Fitting a country to the
         * canvas means every tile overlaps the viewport, and each tile costs a
         * seek across the file — that first redraw took long enough to trip
         * the task watchdog. Zooming out is bounded the same way below. */
        view_span = VIEW_SPAN_DEFAULT;
        int32_t ext = c - a;
        if (ext > 0 && ext < view_span) view_span = ext;
        view_locked = true;
        if (pick_status) lv_label_set_text(pick_status, gimg_describe());
    } else if (pick_status) {
        lv_label_set_text(pick_status, "Not a usable Garmin map");
    }
    map_btn_update();
    return vmap_ready;
}

/* Load whatever was chosen last time, or the only map there is. */
static void vmap_try_load(void)
{
    if (vmap_tried) return;
    vmap_tried = true;

    map_draw_set_contours(mapstore_get_contours());

    char want[MAPSTORE_NAME_MAX];
    mapstore_get_default(want, sizeof(want));

    if (!want[0]) {
        mapstore_entry_t e[MAPSTORE_MAX];
        int n = mapstore_list(e, MAPSTORE_MAX);
        if (n <= 0) {
            Serial.println("[MAP] nothing in " MAPSTORE_DIR ", using built-in coastline");
            return;
        }
        /* Prefer one already unpacked, so first run does not stall on a zip. */
        int pickIdx = 0;
        for (int i = 0; i < n; i++) if (e[i].unpacked) { pickIdx = i; break; }
        snprintf(want, sizeof(want), "%s", e[pickIdx].name);
    }
    vmap_open_named(want);
}

static void vmap_feature_cb(uint8_t type, uint8_t kind,
                            const int32_t *lat, const int32_t *lon, int n, void *user)
{
    map_draw_feature(type, kind, lat, lon, n);
    view_feats++;
}

/* Keep the view on the fix while locked, and inside the map otherwise. */
static void vmap_centre(void)
{
    if (view_locked && has_fix) {
        view_lat = (int32_t)(cur_lat * 1e7);
        view_lon = (int32_t)(cur_lng * 1e7);
    }
}

static void draw_vector_map(void)
{
    if (!map_canvas || !map_buf) return;

    vmap_centre();

    /* Clear through LVGL so the status strip below the map area is cleared
     * too. map_draw_clear() only covers the map itself, which left the strip
     * as allocated — zero, which is black at LV_COLOR_DEPTH 1 — so it showed
     * as a black bar with black-on-black text in it. */
    lv_canvas_fill_bg(map_canvas, lv_color_white(), LV_OPA_COVER);

    map_draw_begin((uint8_t *)map_buf, MAP_W, MAP_H,
                   view_lat, view_lon, view_span,
                   lv_color_black().full, lv_color_white().full);

    /* Half a canvas of margin so features crossing the edge still draw. */
    int32_t half_lat = view_span / 2;
    int32_t half_lon = (int32_t)((int64_t)view_span * MAP_W / (MAP_H * 2));
    /* Longitude degrees are shorter than latitude ones away from the equator,
     * so the query box has to be wider than the plain aspect ratio suggests. */
    float coslat = cosf(view_lat / 1e7f * 0.017453293f);
    if (coslat < 0.02f) coslat = 0.02f;
    half_lon = (int32_t)(half_lon / coslat);

    int32_t qs = view_lat - half_lat, qn = view_lat + half_lat;
    int32_t qw = view_lon - half_lon, qe = view_lon + half_lon;

    uint32_t t0 = millis();
    view_level = gimg_pick_level(qs, qw, qn, qe);
    view_feats = 0;
    gimg_query(view_level, qs, qw, qn, qe, vmap_feature_cb, NULL);
    uint32_t t_query = millis() - t0;

    /* The recorded track on top of the map, solid and thick so it reads
     * against the contour hatching. */
    if (track.size() >= 2) {
        static int32_t tl[64], tn[64];
        size_t i = 0;
        while (i < track.size()) {
            int k = 0;
            /* One point of overlap so consecutive runs join up. */
            if (i) { tl[k] = (int32_t)(track[i - 1].lat * 1e7);
                     tn[k] = (int32_t)(track[i - 1].lng * 1e7); k++; }
            while (k < 64 && i < track.size()) {
                tl[k] = (int32_t)(track[i].lat * 1e7);
                tn[k] = (int32_t)(track[i].lng * 1e7);
                k++; i++;
            }
            map_draw_track(tl, tn, k, 2);
        }
    }

    if (has_fix)
        map_draw_marker((int32_t)(cur_lat * 1e7), (int32_t)(cur_lng * 1e7), 4);

    map_draw_centre_mark(3);
    map_draw_north();          /* the "N" under it is drawn with the text below */

    char sb[16] = "";
    map_draw_scale_bar(sb, sizeof(sb));

    lv_obj_invalidate(map_canvas);

    /* Status strip under the canvas, drawn through LVGL because it is text. */
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.color = lv_color_black();
    ld.font = &lv_font_montserrat_14;

    /* Scale and level only — the overview page already carries the position,
     * and repeating it here just spent the strip twice. */
    char buf[96];
    snprintf(buf, sizeof(buf), "%s  L%d%s", sb[0] ? sb : "-", view_level,
             view_locked ? "  GPS" : "");
    lv_canvas_draw_text(map_canvas, 2, MAP_H + 1, MAP_W - 4, &ld, buf);
    lv_canvas_draw_text(map_canvas, MAP_W - 15, 26, 14, &ld, "N");

    Serial.printf("[MAP] L%d %d feat, %lu seg, query %lu ms\n",
                  view_level, view_feats, map_draw_segments(),
                  (unsigned long)t_query);
}


/* ---- map picker ---- */

/* The on-screen controls route through the same handler as the keys, so the
 * two can never drift apart. */
static void map_ctrl_cb(lv_event_t *e)
{
    char key = (char)(intptr_t)lv_event_get_user_data(e);
    if (vmap_key(key)) {
        draw_vector_map();
        ui_disp_full_refr();
    }
}

static void pick_close(void)
{
    pick_open = false;
    if (pick_ovl) lv_obj_add_flag(pick_ovl, LV_OBJ_FLAG_HIDDEN);
    ui_disp_full_refr();
}

static void pick_choose_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= pick_count) return;

    /* Remember the choice even if opening fails, so the message on the next
     * try is about the same map rather than silently a different one. */
    mapstore_set_default(pick_items[idx].name);
    if (vmap_open_named(pick_items[idx].name)) {
        pick_close();
        if (gps_page == 1) draw_vector_map();
        ui_disp_full_refr();
    } else {
        pick_refresh();          /* leave the list up with the error showing */
        ui_disp_full_refr();
    }
}

static void pick_refresh(void)
{
    if (!pick_list) return;
    lv_obj_clean(pick_list);

    pick_count = mapstore_list(pick_items, MAPSTORE_MAX);
    if (pick_count <= 0) {
        /* Tell the two apart: an unmounted card and an empty folder look the
         * same in a list, and only one of them is fixed by copying files. */
        lv_obj_t *b = lv_list_add_btn(pick_list, NULL,
            pick_count < 0 ? "SD card not mounted.\nRe-seat it, or format FAT32\n(exFAT is not supported)."
                           : "No maps.\nPut a .img in " MAPSTORE_DIR);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
        pick_count = 0;
        return;
    }

    char cur[MAPSTORE_NAME_MAX];
    mapstore_get_default(cur, sizeof(cur));

    for (int i = 0; i < pick_count; i++) {
        char label[96];
        /* Say which zips still need unpacking: it is the difference between
         * opening at once and waiting. */
        snprintf(label, sizeof(label), "%s%s  %luMB%s",
                 strcmp(cur, pick_items[i].name) == 0 ? LV_SYMBOL_OK " " : "",
                 pick_items[i].name,
                 (unsigned long)(pick_items[i].size / (1024 * 1024)),
                 pick_items[i].is_zip
                     ? (pick_items[i].unpacked ? " zip*" : " zip") : "");
        lv_obj_t *b = lv_list_add_btn(pick_list, NULL, label);
        lv_obj_add_event_cb(b, pick_choose_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void pick_open_cb(lv_event_t *e)
{
    if (!pick_ovl) return;
    pick_open = true;
    lv_obj_clear_flag(pick_ovl, LV_OBJ_FLAG_HIDDEN);
    pick_refresh();
    if (pick_status)
        lv_label_set_text(pick_status, vmap_ready ? gimg_describe() : "No map open");
    ui_disp_full_refr();
}

static void map_btn_update(void)
{
    if (map_btn_lbl)
        lv_label_set_text_fmt(map_btn_lbl, LV_SYMBOL_DIRECTORY " %s",
                              vmap_name[0] ? vmap_name : "Select map");
    if (map_ctr_lbl)
        lv_label_set_text(map_ctr_lbl, map_draw_contours() ? "ctr on" : "ctr off");
}

/* w/a/s/d pan by a third of a screen, i/o zoom, g re-lock onto the fix. */
static bool vmap_key(char c)
{
    if (!vmap_ready) return false;
    int32_t step_lat = view_span / 3;
    float coslat = cosf(view_lat / 1e7f * 0.017453293f);
    if (coslat < 0.02f) coslat = 0.02f;
    int32_t step_lon = (int32_t)(((int64_t)view_span * MAP_W / (MAP_H * 3)) / coslat);

    switch (c) {
    case 'w': case 'W': view_lat += step_lat; view_locked = false; break;
    case 's': case 'S': view_lat -= step_lat; view_locked = false; break;
    case 'a': case 'A': view_lon -= step_lon; view_locked = false; break;
    case 'd': case 'D': view_lon += step_lon; view_locked = false; break;
    case 'i': case 'I': view_span = view_span / 2 > 500 ? view_span / 2 : 500; break;
    case 'o': case 'O': {
        int32_t want = view_span < 400000000 ? view_span * 2 : view_span;
        /* Refuse a zoom-out that would span more tiles than a redraw can read.
         * Better a zoom limit than a redraw that never finishes. */
        int32_t hl = want / 2;
        float cl = cosf(view_lat / 1e7f * 0.017453293f);
        if (cl < 0.02f) cl = 0.02f;
        int32_t hn = (int32_t)(((int64_t)want * MAP_W / (MAP_H * 2)) / cl);
        int nt = gimg_overlap_tiles(view_lat - hl, view_lon - hn,
                                    view_lat + hl, view_lon + hn);
        if (nt > VIEW_MAX_TILES) {
            Serial.printf("[MAP] zoom-out refused: would span %d tiles\n", nt);
            return true;                  /* redraw so the scale bar still shows */
        }
        view_span = want;
        break;
    }
    case 'g': case 'G': view_locked = true; break;
    case 'c': case 'C':
        map_draw_set_contours(!map_draw_contours());
        mapstore_set_contours(map_draw_contours());
        map_btn_update();
        break;
    default: return false;
    }

    /* Wrap longitude rather than letting it run off, so panning across the
     * antimeridian works — which is where this map happens to be. */
    if (view_lon >  1800000000) view_lon -= 3600000000LL;
    if (view_lon < -1800000000) view_lon += 3600000000LL;
    if (view_lat >  900000000) view_lat =  900000000;
    if (view_lat < -900000000) view_lat = -900000000;
    return true;
}

static void draw_world_map()
{
    if (!map_canvas) return;

    lv_canvas_fill_bg(map_canvas, lv_color_white(), LV_OPA_COVER);

    /* Draw border */
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_black();
    line_dsc.width = 1;

    lv_point_t border[] = {{0,0},{MAP_W-1,0},{MAP_W-1,WORLD_H-1},{0,WORLD_H-1},{0,0}};
    for (int i = 0; i < 4; i++) {
        lv_point_t pts[2] = {border[i], border[i+1]};
        lv_canvas_draw_line(map_canvas, pts, 2, &line_dsc);
    }

    /* Draw coastlines */
    for (int i = 0; i < (int)COAST_SEGMENTS; i++) {
        int x1 = MAP_W * (coastline[i][1] + 180) / 360;
        int y1 = WORLD_H * (90 - coastline[i][0]) / 180;
        int x2 = MAP_W * (coastline[i][3] + 180) / 360;
        int y2 = WORLD_H * (90 - coastline[i][2]) / 180;
        /* Skip wrap-around segments */
        if (abs(x2 - x1) > MAP_W / 2) continue;
        lv_point_t pts[2] = {{(lv_coord_t)x1,(lv_coord_t)y1},{(lv_coord_t)x2,(lv_coord_t)y2}};
        lv_canvas_draw_line(map_canvas, pts, 2, &line_dsc);
    }

    /* Draw GPS position */
    if (has_fix) {
        int px = MAP_W * (cur_lng + 180) / 360;
        int py = WORLD_H * (90 - cur_lat) / 180;
        /* Draw crosshair */
        for (int d = -4; d <= 4; d++) {
            if (px+d >= 0 && px+d < MAP_W) lv_canvas_set_px(map_canvas, px+d, py, lv_color_black());
            if (py+d >= 0 && py+d < WORLD_H) lv_canvas_set_px(map_canvas, px, py+d, lv_color_black());
        }
        /* Thick cross */
        for (int d = -3; d <= 3; d++) {
            if (px+d >= 0 && px+d < MAP_W && py-1 >= 0) lv_canvas_set_px(map_canvas, px+d, py-1, lv_color_black());
            if (px+d >= 0 && px+d < MAP_W && py+1 < WORLD_H) lv_canvas_set_px(map_canvas, px+d, py+1, lv_color_black());
            if (py+d >= 0 && py+d < WORLD_H && px-1 >= 0) lv_canvas_set_px(map_canvas, px-1, py+d, lv_color_black());
            if (py+d >= 0 && py+d < WORLD_H && px+1 < MAP_W) lv_canvas_set_px(map_canvas, px+1, py+d, lv_color_black());
        }
    }

    /* Coordinate text below map */
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = lv_color_black();
    label_dsc.font = &lv_font_montserrat_14;

    char buf[64];
    if (has_fix)
        snprintf(buf, sizeof(buf), "%.4f, %.4f", cur_lat, cur_lng);
    else
        snprintf(buf, sizeof(buf), "No fix");
    lv_canvas_draw_text(map_canvas, 4, WORLD_H + 4, MAP_W, &label_dsc, buf);
}

/* ---- Page 3: Tracker ---- */

/* The track is drawn on an equirectangular projection about its own centre.
 * Both axes are converted to metres before scaling and share one scale
 * factor, so a pixel is the same distance whichever way you measure it and
 * the plot has the shape of the route rather than of the canvas. North is up
 * by construction: screen y grows downward, so it carries minus the northing.
 *
 * The previous version stretched latitude and longitude independently to fill
 * the box, which squashed the aspect and — worse — changed it as the track
 * grew, so the same route appeared to rotate between refreshes. */

#define TRACK_PAD_PX      8       /* margin kept clear around the track     */
#define TRACK_MIN_SPAN_M  20.0    /* zoom floor: below this a parked        */
                                  /* receiver's noise would fill the canvas */

/* Metres per degree. Latitude is near enough constant; longitude shrinks
 * with the cosine of the latitude, which is what makes the two axes
 * comparable in the first place. */
#define M_PER_DEG_LAT 110574.0
#define M_PER_DEG_LNG 111320.0

static double trk_scale;                  /* pixels per metre */
static double trk_lat0, trk_lng0;         /* projection centre */
static double trk_coslat;

static void trk_project(double lat, double lng, int *x, int *y)
{
    double east  = (lng - trk_lng0) * M_PER_DEG_LNG * trk_coslat;
    double north = (lat - trk_lat0) * M_PER_DEG_LAT;
    *x = (int)lround(TRACK_VIEW_W / 2.0 + east  * trk_scale);
    *y = (int)lround(TRACK_VIEW_H / 2.0 - north * trk_scale);
}

static void trk_px(int x, int y)
{
    if (x >= 0 && x < TRACK_VIEW_W && y >= 0 && y < TRACK_VIEW_H)
        lv_canvas_set_px(track_canvas, x, y, lv_color_black());
}

/* A north arrow, top-right. With the aspect now honest the orientation is
 * worth stating rather than leaving the user to infer it. */
static void draw_north_arrow(void)
{
    const int x = TRACK_VIEW_W - 12, y0 = 20, y1 = 40;

    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_black();
    d.width = 1;
    lv_point_t shaft[2] = {{(lv_coord_t)x, (lv_coord_t)y0}, {(lv_coord_t)x, (lv_coord_t)y1}};
    lv_canvas_draw_line(track_canvas, shaft, 2, &d);

    for (int i = 0; i <= 4; i++) {        /* arrowhead */
        trk_px(x - i, y0 + i);
        trk_px(x + i, y0 + i);
    }

    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.color = lv_color_black();
    ld.font  = &lv_font_montserrat_14;
    lv_canvas_draw_text(track_canvas, x - 5, y1 + 1, 12, &ld, "N");
}

/* A scale bar, bottom-left. Only meaningful because both axes now share a
 * scale — which is the point of showing it. */
static void draw_scale_bar(void)
{
    static const double nice[] = {10, 20, 50, 100, 200, 500,
                                  1000, 2000, 5000, 10000, 20000, 50000};
    const int max_px = 70;

    double metres = nice[0];
    for (unsigned i = 0; i < sizeof(nice) / sizeof(nice[0]); i++) {
        if (nice[i] * trk_scale <= max_px) metres = nice[i];
        else break;
    }
    int len = (int)lround(metres * trk_scale);
    if (len < 4) return;                  /* too zoomed out to say anything */

    const int x0 = 6, y = TRACK_VIEW_H - 8;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_black();
    d.width = 1;
    lv_point_t bar[2] = {{(lv_coord_t)x0, (lv_coord_t)y},
                         {(lv_coord_t)(x0 + len), (lv_coord_t)y}};
    lv_canvas_draw_line(track_canvas, bar, 2, &d);
    for (int t = 0; t < 3; t++) {         /* end ticks */
        trk_px(x0, y - t);
        trk_px(x0 + len, y - t);
    }

    char buf[16];
    if (metres >= 1000) snprintf(buf, sizeof(buf), "%gkm", metres / 1000);
    else                snprintf(buf, sizeof(buf), "%gm", metres);

    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.color = lv_color_black();
    ld.font  = &lv_font_montserrat_14;
    lv_canvas_draw_text(track_canvas, x0 + len + 4, y - 9, 60, &ld, buf);
}

static void draw_track()
{
    if (!track_canvas) return;

    lv_canvas_fill_bg(track_canvas, lv_color_white(), LV_OPA_COVER);

    if (track.size() < 2) {
        lv_draw_label_dsc_t ld;
        lv_draw_label_dsc_init(&ld);
        ld.color = lv_color_black();
        ld.font = &lv_font_montserrat_14;
        lv_canvas_draw_text(track_canvas, 20, 80, 180, &ld,
            tracking ? "Recording...\nWaiting for points" : "Press S to start");
        return;
    }

    /* Bounding box, then project about its centre. */
    double min_lat = 90, max_lat = -90, min_lng = 180, max_lng = -180;
    for (auto &p : track) {
        if (p.lat < min_lat) min_lat = p.lat;
        if (p.lat > max_lat) max_lat = p.lat;
        if (p.lng < min_lng) min_lng = p.lng;
        if (p.lng > max_lng) max_lng = p.lng;
    }

    trk_lat0  = (min_lat + max_lat) / 2;
    trk_lng0  = (min_lng + max_lng) / 2;
    trk_coslat = cos(trk_lat0 * M_PI / 180.0);
    if (trk_coslat < 0.01) trk_coslat = 0.01;      /* keep the poles finite */

    double span_x = (max_lng - min_lng) * M_PER_DEG_LNG * trk_coslat;
    double span_y = (max_lat - min_lat) * M_PER_DEG_LAT;
    if (span_x < TRACK_MIN_SPAN_M) span_x = TRACK_MIN_SPAN_M;
    if (span_y < TRACK_MIN_SPAN_M) span_y = TRACK_MIN_SPAN_M;

    /* One scale for both axes: whichever direction runs out of room first. */
    double sx_fit = (TRACK_VIEW_W - 2.0 * TRACK_PAD_PX) / span_x;
    double sy_fit = (TRACK_VIEW_H - 2.0 * TRACK_PAD_PX) / span_y;
    trk_scale = sx_fit < sy_fit ? sx_fit : sy_fit;

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_black();
    line_dsc.width = 2;

    int px = 0, py = 0;
    trk_project(track[0].lat, track[0].lng, &px, &py);
    for (size_t i = 1; i < track.size(); i++) {
        int x, y;
        trk_project(track[i].lat, track[i].lng, &x, &y);
        lv_point_t pts[2] = {{(lv_coord_t)px, (lv_coord_t)py},
                             {(lv_coord_t)x,  (lv_coord_t)y}};
        lv_canvas_draw_line(track_canvas, pts, 2, &line_dsc);
        px = x; py = y;
    }

    /* Start: open circle. */
    int sx, sy;
    trk_project(track[0].lat, track[0].lng, &sx, &sy);
    for (int a = 0; a < 360; a += 15)
        trk_px(sx + (int)lround(3 * cos(a * M_PI / 180)),
               sy + (int)lround(3 * sin(a * M_PI / 180)));

    /* End: filled square. */
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++)
            trk_px(px + dx, py + dy);

    draw_north_arrow();
    draw_scale_bar();
}

static void update_track_info()
{
    if (!lbl_track_info) return;
    if (!tracking && track.empty()) {
        lv_label_set_text(lbl_track_info, "S: start tracking");
        return;
    }
    if (tracking && !has_fix) {
        lv_label_set_text(lbl_track_info, "ARMED  waiting for fix...  S: stop");
        return;
    }
    uint32_t elapsed = tracking ? (millis() - track_start_ms) / 1000 : 0;
    if (!tracking && !track.empty()) {
        elapsed = (track.back().ms - track.front().ms) / 1000;
    }
    int h = elapsed / 3600, m = (elapsed % 3600) / 60, s = elapsed % 60;
    if (!tracking && last_gpx_path[0]) {
        lv_label_set_text_fmt(lbl_track_info,
            "STOP  Dist: %.0fm  Pts: %d\nSaved: %s",
            track_dist_m, (int)track.size(), last_gpx_path);
    } else {
        lv_label_set_text_fmt(lbl_track_info, "%s  Time: %02d:%02d:%02d  Dist: %.0fm  Pts: %d",
            tracking ? "REC" : "STOP", h, m, s, track_dist_m, (int)track.size());
    }
}

/* Convert a UTC calendar date/time to a Unix epoch without relying on timegm()
 * (days-from-civil, Howard Hinnant's algorithm). */
static time_t utc_to_epoch(int y, int mo, int d, int h, int mi, int s)
{
    y -= (mo <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + (long)doe - 719468L;
    return (time_t)days * 86400 + h * 3600 + mi * 60 + s;
}

/* Write the current track to /gpx/track_YYYYMMDD_HHMMSS.gpx on the SD card.
 * Returns true on success; stores the path in last_gpx_path. */
static bool save_gpx()
{
    if (track.empty()) return false;

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);

    if (!SD.exists(GPX_DIR)) SD.mkdir(GPX_DIR);

    /* Name the file from the start time when we have one, else a millis stamp. */
    char path[64];
    if (track_base_epoch != 0) {
        struct tm tm;
        gmtime_r(&track_base_epoch, &tm);
        snprintf(path, sizeof(path), GPX_DIR "/track_%04d%02d%02d_%02d%02d%02d.gpx",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        snprintf(path, sizeof(path), GPX_DIR "/track_%lu.gpx", (unsigned long)track_start_ms);
    }

    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        shared_spi_unlock();
        Serial.printf("[GPS] GPX open failed: %s\n", path);
        return false;
    }

    f.print("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<gpx version=\"1.1\" creator=\"T-Deck-Pro\" "
            "xmlns=\"http://www.topografix.com/GPX/1/1\">\n"
            "<trk><name>T-Deck-Pro Track</name><trkseg>\n");

    for (auto &p : track) {
        f.printf("<trkpt lat=\"%.6f\" lon=\"%.6f\">", p.lat, p.lng);
        if (track_base_epoch != 0) {
            time_t pt = track_base_epoch + (time_t)((p.ms - track_start_ms) / 1000);
            struct tm tm;
            gmtime_r(&pt, &tm);
            char ts[32];
            strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
            f.printf("<time>%s</time>", ts);
        }
        f.print("</trkpt>\n");
    }

    f.print("</trkseg></trk></gpx>\n");
    f.close();
    shared_spi_unlock();

    strncpy(last_gpx_path, path, sizeof(last_gpx_path) - 1);
    last_gpx_path[sizeof(last_gpx_path) - 1] = '\0';
    track_saved = true;
    Serial.printf("[GPS] GPX saved to SD: %s (%d points, %.0fm)\n",
                  path, (int)track.size(), track_dist_m);
    return true;
}

static void track_toggle()
{
    if (tracking) {
        tracking = false;      /* releases the idle-mode hold */
        Serial.printf("[GPS] Track stopped: %d points, %.0fm\n", (int)track.size(), track_dist_m);
        save_gpx();
    } else {
        /* Arm even without a fix rather than ignoring the key. Refusing in
         * silence made the receiver's own slowness look like a dead keyboard;
         * track_record_point() already waits for a fix before logging. */
        track.clear();
        track_dist_m = 0;
        track_start_ms = millis();
        track_last_pt_ms = 0;
        track_saved = false;
        last_gpx_path[0] = '\0';
        /* Anchor wall-clock time from the GPS UTC fix (used for GPX <time>). */
        track_base_epoch = (cur_year >= 2000)
            ? utc_to_epoch(cur_year, cur_month, cur_day, cur_hour, cur_min, cur_sec)
            : 0;
        tracking = true;
        /* From here the idle manager sees us as busy, so the receiver keeps
         * its power and the CPU its clock even with nobody pressing keys. */
        Serial.printf("[GPS] Track %s (GPS held on, idle disabled)\n",
                      has_fix ? "started" : "armed, waiting for fix");
    }
}

/* Queried by the idle manager — see ui_gps_enhanced.h. */
bool gps_track_is_recording(void) { return tracking; }

static void track_record_point()
{
    if (!tracking || !has_fix) return;
    uint32_t now = millis();
    if (now - track_last_pt_ms < 5000) return;
    track_last_pt_ms = now;

    if (track.size() > 0) {
        track_dist_m += haversine_m(track.back().lat, track.back().lng, cur_lat, cur_lng);
    }
    if (track.size() < TRACK_MAX) {
        track.push_back({cur_lat, cur_lng, now});
    }
}

/* ---- Page 4: Tracks browser ---- */

/* Count "<trkpt" occurrences in a GPX file (no repeated prefix, so a plain
 * running match with reset-on-mismatch is exact). */
static int count_trkpts(const char *path)
{
    static const char pat[] = "<trkpt";
    const int pl = 6;
    int n = 0, m = 0;
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    File f = SD.open(path, FILE_READ);
    if (f) {
        uint8_t buf[512];
        while (f.available()) {
            int r = f.read(buf, sizeof(buf));
            for (int i = 0; i < r; i++) {
                char c = (char)buf[i];
                if (c == pat[m]) { if (++m == pl) { n++; m = 0; } }
                else             { m = (c == pat[0]) ? 1 : 0; }
            }
        }
        f.close();
    }
    shared_spi_unlock();
    return n;
}

static void gpx_item_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= gpx_count) return;
    gpx_selected = (int)idx;
    if (gpx_del_btn) lv_obj_clear_flag(gpx_del_btn, LV_OBJ_FLAG_HIDDEN);
    const char *base = strrchr(gpx_files[idx], '/');
    base = base ? base + 1 : gpx_files[idx];
    int pts = count_trkpts(gpx_files[idx]);
    if (gpx_info) lv_label_set_text_fmt(gpx_info, "%s\n%d points", base, pts);
    ui_disp_full_refr();
}

static void refresh_gpx_list()
{
    if (!gpx_list) return;
    lv_obj_clean(gpx_list);
    gpx_count = 0;
    gpx_selected = -1;
    if (gpx_del_btn) lv_obj_add_flag(gpx_del_btn, LV_OBJ_FLAG_HIDDEN);

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    if (!SD.exists(GPX_DIR)) SD.mkdir(GPX_DIR);
    File dir = SD.open(GPX_DIR);
    if (dir && dir.isDirectory()) {
        File e = dir.openNextFile();
        while (e && gpx_count < GPX_MAX_FILES) {
            if (!e.isDirectory()) {
                const char *n = e.name();
                const char *base = strrchr(n, '/');
                base = base ? base + 1 : n;
                uint32_t sz = e.size();
                snprintf(gpx_files[gpx_count], sizeof(gpx_files[0]), GPX_DIR "/%s", base);
                char label[64];
                snprintf(label, sizeof(label), "%s (%uK)", base,
                         (unsigned)((sz + 1023) / 1024));
                lv_obj_t *btn = lv_list_add_btn(gpx_list, LV_SYMBOL_FILE, label);
                lv_obj_add_event_cb(btn, gpx_item_cb, LV_EVENT_CLICKED,
                                    (void *)(intptr_t)gpx_count);
                gpx_count++;
            }
            e.close();
            e = dir.openNextFile();
        }
        dir.close();
    }
    shared_spi_unlock();

    if (gpx_info)
        lv_label_set_text(gpx_info, gpx_count ? "Tap a track for details" : "No tracks yet");
}

static void gpx_delete_cb(lv_event_t *e)
{
    if (gpx_selected < 0 || gpx_selected >= gpx_count) return;
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    SD.remove(gpx_files[gpx_selected]);
    shared_spi_unlock();
    refresh_gpx_list();
    ui_disp_full_refr();
}

/* ---- Timer ---- */

static void gps_update_cb(lv_timer_t *t)
{
    ui_gps_get_coord(&cur_lat, &cur_lng);
    ui_gps_get_satellites(&cur_sats);
    ui_gps_get_speed(&cur_speed);
    ui_gps_get_data(&cur_year, &cur_month, &cur_day);
    ui_gps_get_time(&cur_hour, &cur_min, &cur_sec);
    has_fix = (cur_sats > 0 && (cur_lat != 0 || cur_lng != 0));

    track_record_point();

    if (gps_page == 0) update_overview();
    else if (gps_page == 1) { if (vmap_ready) draw_vector_map(); else draw_world_map(); }
    else if (gps_page == 2) { draw_track(); update_track_info(); }
}

/* ---- Keyboard ---- */

void gps_keyboard_poll()
{
    if (!gps_kbd_active) return;
    char c;
    if (!keypad_get_val(&c)) return;
    keypad_set_flag();

    if (pick_open) {
        /* Any key closes the picker rather than paging out from under it. */
        pick_close();
        return;
    }

    if (c == '\b') {
        if (gps_page > 0) show_gps_page(gps_page - 1);
        else { gps_kbd_active = false; scr_mgr_pop(false); }
    } else if (c == '\n' || c == ' ') {
        show_gps_page((gps_page + 1) % GPS_PAGE_COUNT);
    } else if (gps_page == 1 && vmap_key(c)) {
        draw_vector_map();            /* pan/zoom repaints immediately */
        ui_disp_full_refr();
    } else if ((c == 's' || c == 'S') && gps_page == 2) {
        track_toggle();               /* shifted S counts too */
    }
}

/* ---- Lifecycle ---- */

static void gps_back_cb(lv_event_t *e) { gps_kbd_active = false; scr_mgr_pop(false); }

static lv_obj_t *make_page(lv_obj_t *parent)
{
    lv_obj_t *pg = lv_obj_create(parent);
    lv_obj_set_size(pg, 236, 266);
    lv_obj_align(pg, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_border_width(pg, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pg, 2, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(pg, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
    return pg;
}

static void gps_create(lv_obj_t *parent)
{
    scr_back_btn_create(parent, "GPS", gps_back_cb);

    page_ind = lv_label_create(parent);
    lv_obj_set_style_text_font(page_ind, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(page_ind, LV_ALIGN_BOTTOM_MID, 0, -4);

    /* Page 0: Overview */
    pages[0] = make_page(parent);
    lv_obj_set_flex_flow(pages[0], LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pages[0], 2, LV_PART_MAIN);

    lbl_overview = lv_label_create(pages[0]);
    lv_obj_set_width(lbl_overview, lv_pct(100));
    lv_obj_set_style_text_font(lbl_overview, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_label_set_long_mode(lbl_overview, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_overview, "Waiting for GPS...");

    /* Page 1: World map */
    pages[1] = make_page(parent);
    map_buf = (lv_color_t *)ps_calloc(MAP_W * (MAP_H + MAP_STATUS_H), sizeof(lv_color_t));
    if (map_buf) {
        map_canvas = lv_canvas_create(pages[1]);
        lv_canvas_set_buffer(map_canvas, map_buf, MAP_W, MAP_H + MAP_STATUS_H, LV_IMG_CF_TRUE_COLOR);
        lv_obj_align(map_canvas, LV_ALIGN_TOP_MID, 0, 0);
    }

    /* Map chooser. Lives in the bottom strip the page indicator otherwise
     * occupies, rather than costing the map a row of its own — the indicator
     * is hidden while this page is up, since the button says where you are
     * just as well. */
    map_btn = lv_btn_create(parent);
    lv_obj_set_size(map_btn, 232, 26);
    lv_obj_align(map_btn, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_add_flag(map_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *mbtn = map_btn;
    lv_obj_set_style_radius(mbtn, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(mbtn, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(mbtn, lv_color_white(), LV_PART_MAIN);
    lv_obj_add_event_cb(mbtn, pick_open_cb, LV_EVENT_CLICKED, NULL);
    /* Name and contour state as separate labels: the name is elided when it is
     * long, and a status tacked onto the same string would be elided with it. */
    map_btn_lbl = lv_label_create(mbtn);
    lv_label_set_long_mode(map_btn_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(map_btn_lbl, 168);
    lv_obj_align(map_btn_lbl, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_text_color(map_btn_lbl, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(map_btn_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(map_btn_lbl, LV_SYMBOL_DIRECTORY " Select map");

    map_ctr_lbl = lv_label_create(mbtn);
    lv_obj_align(map_ctr_lbl, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_text_color(map_ctr_lbl, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(map_ctr_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(map_ctr_lbl, "");

    /* Zoom and recentre, overlaid on the right edge of the map. The keys
     * (i/o to zoom, wasd to pan, g to re-lock) still work, but nothing on
     * screen said so. */
    struct { const char *txt; int dy; char key; } mc[] = {
        { LV_SYMBOL_PLUS,  4,   'i' },
        { LV_SYMBOL_MINUS, 36,  'o' },
        { LV_SYMBOL_GPS,   68,  'g' },
    };
    for (unsigned i = 0; i < sizeof(mc) / sizeof(mc[0]); i++) {
        lv_obj_t *b = lv_btn_create(pages[1]);
        lv_obj_set_size(b, 28, 28);
        lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -6, mc[i].dy);
        lv_obj_set_style_radius(b, 4, LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_add_event_cb(b, map_ctrl_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)mc[i].key);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, mc[i].txt);
        lv_obj_set_style_text_color(l, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_center(l);
    }

    /* Picker overlay: created on the screen, not the page, so it covers the
     * whole app area and a stray tap cannot reach the map behind it. */
    pick_ovl = lv_obj_create(parent);
    lv_obj_set_size(pick_ovl, 240, 292);
    lv_obj_align(pick_ovl, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_radius(pick_ovl, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pick_ovl, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(pick_ovl, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pick_ovl, 4, LV_PART_MAIN);
    lv_obj_clear_flag(pick_ovl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pick_ovl, LV_OBJ_FLAG_HIDDEN);

    pick_list = lv_list_create(pick_ovl);
    lv_obj_set_width(pick_list, lv_pct(100));
    lv_obj_set_flex_grow(pick_list, 1);
    lv_obj_set_style_pad_all(pick_list, 0, LV_PART_MAIN);

    pick_status = lv_label_create(pick_ovl);
    lv_obj_set_width(pick_status, lv_pct(100));
    lv_label_set_long_mode(pick_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(pick_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(pick_status, lv_palette_main(LV_PALETTE_GREY), LV_PART_MAIN);
    lv_label_set_text(pick_status, "");

    /* Page 2: Tracker */
    pages[2] = make_page(parent);
    lv_obj_set_flex_flow(pages[2], LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pages[2], 2, LV_PART_MAIN);

    lbl_track_info = lv_label_create(pages[2]);
    lv_obj_set_width(lbl_track_info, lv_pct(100));
    lv_obj_set_style_text_font(lbl_track_info, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(lbl_track_info, "S: start tracking");

    track_buf = (lv_color_t *)ps_calloc(TRACK_VIEW_W * TRACK_VIEW_H, sizeof(lv_color_t));
    if (track_buf) {
        track_canvas = lv_canvas_create(pages[2]);
        lv_canvas_set_buffer(track_canvas, track_buf, TRACK_VIEW_W, TRACK_VIEW_H, LV_IMG_CF_TRUE_COLOR);
    }

    /* Page 3: Tracks browser (saved GPX files) */
    pages[3] = make_page(parent);

    gpx_list = lv_list_create(pages[3]);
    lv_obj_set_size(gpx_list, 228, 198);
    lv_obj_align(gpx_list, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(gpx_list, 2, LV_PART_MAIN);

    gpx_info = lv_label_create(pages[3]);
    lv_obj_set_width(gpx_info, 150);
    lv_label_set_long_mode(gpx_info, LV_LABEL_LONG_WRAP);
    lv_obj_align(gpx_info, LV_ALIGN_BOTTOM_LEFT, 2, -4);
    lv_obj_set_style_text_font(gpx_info, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(gpx_info, "No tracks yet");

    gpx_del_btn = lv_btn_create(pages[3]);
    lv_obj_set_size(gpx_del_btn, 72, 28);
    lv_obj_align(gpx_del_btn, LV_ALIGN_BOTTOM_RIGHT, -2, -4);
    lv_obj_set_style_radius(gpx_del_btn, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(gpx_del_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(gpx_del_btn, lv_color_white(), LV_PART_MAIN);
    lv_obj_add_event_cb(gpx_del_btn, gpx_delete_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(gpx_del_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *dl = lv_label_create(gpx_del_btn);
    lv_label_set_text(dl, LV_SYMBOL_TRASH " Del");
    lv_obj_set_style_text_color(dl, lv_color_black(), LV_PART_MAIN);
    lv_obj_center(dl);

    show_gps_page(0);
    vmap_try_load();
    map_btn_update();
    gps_timer = lv_timer_create(gps_update_cb, 3000, NULL);
    gps_kbd_active = true;
}

static void gps_entry(void)
{
    /* Receiver LDO + NMEA task; both drop again on exit. */
    power_acquire(PWR_GPS);
    ui_disp_full_refr();
}
static void gps_exit(void)
{
    power_release(PWR_GPS);
    if (gps_timer) { lv_timer_del(gps_timer); gps_timer = NULL; }
    ui_disp_full_refr();
}
static void gps_destroy(void)
{
    /* Persist the track if the user left the screen without pressing stop. */
    if (!track.empty() && !track_saved) save_gpx();
    gps_kbd_active = false;
    tracking = false;
    if (gps_timer) { lv_timer_del(gps_timer); gps_timer = NULL; }
    lbl_overview = lbl_track_info = map_canvas = track_canvas = page_ind = NULL;
    pick_ovl = pick_list = pick_status = map_btn = map_btn_lbl = map_ctr_lbl = NULL;
    pick_open = false;
    pick_count = 0;
    gpx_list = gpx_info = gpx_del_btn = NULL;
    gpx_count = 0;
    gpx_selected = -1;
    for (int i = 0; i < GPS_PAGE_COUNT; i++) pages[i] = NULL;
    if (map_buf) { free(map_buf); map_buf = NULL; }
    if (vmap_ready) { gimg_close(); vmap_ready = false; }
    vmap_tried = false;
    if (track_buf) { free(track_buf); track_buf = NULL; }
    track.clear();
}

scr_lifecycle_t screen_gps_enhanced = {
    .create = gps_create,
    .entry = gps_entry,
    .exit = gps_exit,
    .destroy = gps_destroy,
};
