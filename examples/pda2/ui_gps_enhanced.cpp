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
#define MAP_H 120
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

/* Page 2: world map canvas */
static lv_obj_t *map_canvas = NULL;
static lv_color_t *map_buf = NULL;

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

static void show_gps_page(int pg)
{
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

/* Simplified world coastline — major continent outlines as lat/lon polylines */
/* Each segment: {lat1, lon1, lat2, lon2} */
static const int16_t coastline[][4] = {
    /* North America outline */
    {72,-168, 71,-156}, {71,-156, 65,-168}, {65,-168, 60,-145}, {60,-145, 55,-130},
    {55,-130, 49,-125}, {49,-125, 48,-124}, {48,-124, 38,-122}, {38,-122, 32,-117},
    {32,-117, 25,-110}, {25,-110, 19,-105}, {19,-105, 15,-92}, {15,-92, 18,-88},
    {18,-88, 21,-87}, {21,-87, 26,-82}, {26,-82, 30,-81}, {30,-81, 35,-75},
    {35,-75, 40,-74}, {40,-74, 42,-70}, {42,-70, 45,-67}, {45,-67, 47,-60},
    {47,-60, 52,-56}, {52,-56, 55,-60}, {55,-60, 60,-64}, {60,-64, 65,-64},
    {65,-64, 72,-70}, {72,-70, 72,-168},
    /* South America */
    {12,-72, 8,-77}, {8,-77, 4,-77}, {4,-77, -5,-80}, {-5,-80, -15,-75},
    {-15,-75, -23,-70}, {-23,-70, -33,-71}, {-33,-71, -42,-65}, {-42,-65, -50,-68},
    {-50,-68, -55,-68}, {-55,-68, -55,-66}, {-55,-66, -42,-63}, {-42,-63, -35,-56},
    {-35,-56, -25,-48}, {-25,-48, -23,-41}, {-23,-41, -12,-38}, {-12,-38, -5,-35},
    {-5,-35, 2,-50}, {2,-50, 7,-60}, {7,-60, 11,-72}, {11,-72, 12,-72},
    /* Europe */
    {36,-10, 38,-9}, {38,-9, 43,-9}, {43,-9, 48,-5}, {48,-5, 51,2},
    {51,2, 54,8}, {54,8, 57,10}, {57,10, 60,5}, {60,5, 64,14},
    {64,14, 70,20}, {70,20, 71,28}, {71,28, 70,40},
    /* Africa */
    {36,-6, 32,-5}, {32,-5, 28,-13}, {28,-13, 22,-17}, {22,-17, 15,-17},
    {15,-17, 5,-5}, {5,-5, 4,9}, {4,9, -5,12}, {-5,12, -12,25},
    {-12,25, -23,30}, {-23,30, -26,33}, {-26,33, -34,26}, {-34,26, -34,18},
    {-34,18, -30,17}, {-30,17, -18,12}, {-18,12, -12,44}, {-12,44, 2,45},
    {2,45, 12,44}, {12,44, 15,42}, {15,42, 28,34}, {28,34, 32,32},
    {32,32, 36,36}, {36,36, 36,-6},
    /* Asia (simplified) */
    {70,40, 65,60}, {65,60, 55,60}, {55,60, 45,50}, {45,50, 40,44},
    {40,44, 36,36}, {36,36, 32,35}, {32,35, 30,48}, {30,48, 25,57},
    {25,57, 22,60}, {22,60, 8,77}, {8,77, 20,90}, {20,90, 22,100},
    {22,100, 20,107}, {20,107, 30,122}, {30,122, 40,120}, {40,120, 42,132},
    {42,132, 50,140}, {50,140, 55,138}, {55,138, 60,150}, {60,150, 63,172},
    {63,172, 66,180}, {66,180, 70,180}, {70,180, 72,140}, {72,140, 70,40},
    /* Australia */
    {-12,132, -18,122}, {-18,122, -22,114}, {-22,114, -28,114}, {-28,114, -35,118},
    {-35,118, -38,145}, {-38,145, -35,151}, {-35,151, -28,153}, {-28,153, -20,149},
    {-20,149, -15,145}, {-15,145, -12,136}, {-12,136, -12,132},
};
#define COAST_SEGMENTS (sizeof(coastline) / sizeof(coastline[0]))

static void draw_world_map()
{
    if (!map_canvas) return;

    lv_canvas_fill_bg(map_canvas, lv_color_white(), LV_OPA_COVER);

    /* Draw border */
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_black();
    line_dsc.width = 1;

    lv_point_t border[] = {{0,0},{MAP_W-1,0},{MAP_W-1,MAP_H-1},{0,MAP_H-1},{0,0}};
    for (int i = 0; i < 4; i++) {
        lv_point_t pts[2] = {border[i], border[i+1]};
        lv_canvas_draw_line(map_canvas, pts, 2, &line_dsc);
    }

    /* Draw coastlines */
    for (int i = 0; i < (int)COAST_SEGMENTS; i++) {
        int x1 = MAP_W * (coastline[i][1] + 180) / 360;
        int y1 = MAP_H * (90 - coastline[i][0]) / 180;
        int x2 = MAP_W * (coastline[i][3] + 180) / 360;
        int y2 = MAP_H * (90 - coastline[i][2]) / 180;
        /* Skip wrap-around segments */
        if (abs(x2 - x1) > MAP_W / 2) continue;
        lv_point_t pts[2] = {{(lv_coord_t)x1,(lv_coord_t)y1},{(lv_coord_t)x2,(lv_coord_t)y2}};
        lv_canvas_draw_line(map_canvas, pts, 2, &line_dsc);
    }

    /* Draw GPS position */
    if (has_fix) {
        int px = MAP_W * (cur_lng + 180) / 360;
        int py = MAP_H * (90 - cur_lat) / 180;
        /* Draw crosshair */
        for (int d = -4; d <= 4; d++) {
            if (px+d >= 0 && px+d < MAP_W) lv_canvas_set_px(map_canvas, px+d, py, lv_color_black());
            if (py+d >= 0 && py+d < MAP_H) lv_canvas_set_px(map_canvas, px, py+d, lv_color_black());
        }
        /* Thick cross */
        for (int d = -3; d <= 3; d++) {
            if (px+d >= 0 && px+d < MAP_W && py-1 >= 0) lv_canvas_set_px(map_canvas, px+d, py-1, lv_color_black());
            if (px+d >= 0 && px+d < MAP_W && py+1 < MAP_H) lv_canvas_set_px(map_canvas, px+d, py+1, lv_color_black());
            if (py+d >= 0 && py+d < MAP_H && px-1 >= 0) lv_canvas_set_px(map_canvas, px-1, py+d, lv_color_black());
            if (py+d >= 0 && py+d < MAP_H && px+1 < MAP_W) lv_canvas_set_px(map_canvas, px+1, py+d, lv_color_black());
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
    lv_canvas_draw_text(map_canvas, 4, MAP_H + 4, MAP_W, &label_dsc, buf);
}

/* ---- Page 3: Tracker ---- */

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

    /* Find bounding box */
    double min_lat = 90, max_lat = -90, min_lng = 180, max_lng = -180;
    for (auto &p : track) {
        if (p.lat < min_lat) min_lat = p.lat;
        if (p.lat > max_lat) max_lat = p.lat;
        if (p.lng < min_lng) min_lng = p.lng;
        if (p.lng > max_lng) max_lng = p.lng;
    }

    double pad = 0.001;
    double dlat = max_lat - min_lat + pad * 2;
    double dlng = max_lng - min_lng + pad * 2;
    min_lat -= pad; min_lng -= pad;

    /* Draw track lines */
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_black();
    line_dsc.width = 2;

    for (size_t i = 1; i < track.size(); i++) {
        int x1 = (int)((track[i-1].lng - min_lng) / dlng * (TRACK_VIEW_W - 10)) + 5;
        int y1 = (int)((max_lat + pad - track[i-1].lat) / dlat * (TRACK_VIEW_H - 10)) + 5;
        int x2 = (int)((track[i].lng - min_lng) / dlng * (TRACK_VIEW_W - 10)) + 5;
        int y2 = (int)((max_lat + pad - track[i].lat) / dlat * (TRACK_VIEW_H - 10)) + 5;
        lv_point_t pts[2] = {{(lv_coord_t)x1,(lv_coord_t)y1},{(lv_coord_t)x2,(lv_coord_t)y2}};
        lv_canvas_draw_line(track_canvas, pts, 2, &line_dsc);
    }

    /* Mark start with circle, end with filled square */
    int sx = (int)((track[0].lng - min_lng) / dlng * (TRACK_VIEW_W - 10)) + 5;
    int sy = (int)((max_lat + pad - track[0].lat) / dlat * (TRACK_VIEW_H - 10)) + 5;
    for (int a = 0; a < 360; a += 15) {
        int cx = sx + (int)(3 * cos(a * M_PI / 180));
        int cy = sy + (int)(3 * sin(a * M_PI / 180));
        if (cx >= 0 && cx < TRACK_VIEW_W && cy >= 0 && cy < TRACK_VIEW_H)
            lv_canvas_set_px(track_canvas, cx, cy, lv_color_black());
    }
}

static void update_track_info()
{
    if (!lbl_track_info) return;
    if (!tracking && track.empty()) {
        lv_label_set_text(lbl_track_info, "S: start tracking");
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
        tracking = false;
        Serial.printf("[GPS] Track stopped: %d points, %.0fm\n", (int)track.size(), track_dist_m);
        save_gpx();
    } else {
        if (!has_fix) return;
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
        Serial.println("[GPS] Track started");
    }
}

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
    else if (gps_page == 1) draw_world_map();
    else if (gps_page == 2) { draw_track(); update_track_info(); }
}

/* ---- Keyboard ---- */

void gps_keyboard_poll()
{
    if (!gps_kbd_active) return;
    char c;
    if (!keypad_get_val(&c)) return;
    keypad_set_flag();

    if (c == '\b') {
        if (gps_page > 0) show_gps_page(gps_page - 1);
        else { gps_kbd_active = false; scr_mgr_pop(false); }
    } else if (c == '\n' || c == ' ') {
        show_gps_page((gps_page + 1) % GPS_PAGE_COUNT);
    } else if (c == 's' && gps_page == 2) {
        track_toggle();
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
    map_buf = (lv_color_t *)ps_calloc(MAP_W * (MAP_H + 30), sizeof(lv_color_t));
    if (map_buf) {
        map_canvas = lv_canvas_create(pages[1]);
        lv_canvas_set_buffer(map_canvas, map_buf, MAP_W, MAP_H + 30, LV_IMG_CF_TRUE_COLOR);
        lv_obj_align(map_canvas, LV_ALIGN_TOP_MID, 0, 0);
    }

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
    gpx_list = gpx_info = gpx_del_btn = NULL;
    gpx_count = 0;
    gpx_selected = -1;
    for (int i = 0; i < GPS_PAGE_COUNT; i++) pages[i] = NULL;
    if (map_buf) { free(map_buf); map_buf = NULL; }
    if (track_buf) { free(track_buf); track_buf = NULL; }
    track.clear();
}

scr_lifecycle_t screen_gps_enhanced = {
    .create = gps_create,
    .entry = gps_entry,
    .exit = gps_exit,
    .destroy = gps_destroy,
};
