/**
 * @file      ui_weather.cpp
 * @brief     Weather app using OpenWeatherMap One Call API 3.0.
 *            Adapted for T-Deck Pro factory framework.
 */
#include "Arduino.h"
#include "ui_deckpro.h"
#include "ui_deckpro_port.h"
#include "power_mgr.h"
#include "city_db.h"
#include "cjk_font.h"
#include "http_utils.h"
#include "config_keys.h"
#include "app_config.h"
#include <cJSON.h>
#include <Preferences.h>
#include <WiFi.h>

LV_IMG_DECLARE(img_w_clear);
LV_IMG_DECLARE(img_w_pcloudy);
LV_IMG_DECLARE(img_w_cloud);
LV_IMG_DECLARE(img_w_rain);
LV_IMG_DECLARE(img_w_storm);
LV_IMG_DECLARE(img_w_snow);
LV_IMG_DECLARE(img_w_mist);

// --- Data ---

struct current_weather_t {
    float temp;
    float feels_like;
    int humidity;
    float wind;
    int pressure;
    char desc[64];
    char icon[8];
    int uvi;
};

struct hourly_entry_t {
    time_t ts;
    char time_str[6];
    char desc[16];
    float temp;
    int pop_pct;
};

struct daily_entry_t {
    char day_str[6];
    char desc[16];
    float temp_min, temp_max;
    int humidity;
    int pop_pct;
};

#define MAX_HOURLY 12
#define MAX_DAILY 8

static current_weather_t cur = {};
static char location_name[64] = "";
static hourly_entry_t hourly[MAX_HOURLY] = {};
static int hourly_count = 0;
static daily_entry_t daily[MAX_DAILY] = {};
static int daily_count = 0;
static int32_t tz_offset = 0;
static bool data_valid = false;
static uint32_t last_fetch_time = 0;

// --- UI state ---
static lv_timer_t *refresh_timer = NULL;
static TaskHandle_t fetch_task = NULL;
static lv_obj_t *city_label = NULL;
static lv_obj_t *temp_label = NULL;
static lv_obj_t *detail_label = NULL;
static lv_obj_t *status_label = NULL;
static lv_obj_t *icon_img = NULL;
static lv_obj_t *hourly_table = NULL;
static lv_obj_t *daily_table = NULL;

/* City picker: an overlay rather than a fourth page, so the existing 3-page
 * cycle is untouched. Empty city_choice means "follow GPS". */
static lv_obj_t *city_ovl = NULL;
static lv_obj_t *city_search_ta = NULL;
static lv_obj_t *city_result_list = NULL;
static lv_obj_t *city_btn_lbl = NULL;
static bool city_picker_open = false;
static bool weather_active = false;

static const lv_img_dsc_t *weather_icon_img(const char *ic)
{
    if (!ic || !ic[0]) return &img_w_cloud;
    char c0 = ic[0], c1 = ic[1];
    if (c0 == '0' && c1 == '1') return &img_w_clear;
    if (c0 == '0' && c1 == '2') return &img_w_pcloudy;
    if (c0 == '0' && (c1 == '3' || c1 == '4')) return &img_w_cloud;
    if (c0 == '0' && c1 == '9') return &img_w_rain;
    if (c0 == '1' && c1 == '0') return &img_w_rain;
    if (c0 == '1' && c1 == '1') return &img_w_storm;
    if (c0 == '1' && c1 == '3') return &img_w_snow;
    if (c0 == '5' && c1 == '0') return &img_w_mist;
    return &img_w_cloud;
}

// --- JSON parsing ---

static void parse_onecall(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;

    const char *day_names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

    cJSON *tz = cJSON_GetObjectItem(root, "timezone_offset");
    if (tz) tz_offset = tz->valueint;

    cJSON *current = cJSON_GetObjectItem(root, "current");
    if (current) {
        cJSON *t = cJSON_GetObjectItem(current, "temp");
        if (t) cur.temp = t->valuedouble;
        cJSON *fl = cJSON_GetObjectItem(current, "feels_like");
        if (fl) cur.feels_like = fl->valuedouble;
        cJSON *hum = cJSON_GetObjectItem(current, "humidity");
        if (hum) cur.humidity = hum->valueint;
        cJSON *pres = cJSON_GetObjectItem(current, "pressure");
        if (pres) cur.pressure = pres->valueint;
        cJSON *ws = cJSON_GetObjectItem(current, "wind_speed");
        if (ws) cur.wind = ws->valuedouble;
        cJSON *uvi = cJSON_GetObjectItem(current, "uvi");
        if (uvi) cur.uvi = (int)uvi->valuedouble;

        cJSON *wa = cJSON_GetObjectItem(current, "weather");
        if (wa && cJSON_GetArraySize(wa) > 0) {
            cJSON *w0 = cJSON_GetArrayItem(wa, 0);
            cJSON *desc = cJSON_GetObjectItem(w0, "description");
            if (desc && desc->valuestring) strncpy(cur.desc, desc->valuestring, 63);
            cJSON *ic = cJSON_GetObjectItem(w0, "icon");
            if (ic && ic->valuestring) strncpy(cur.icon, ic->valuestring, 7);
        }
        data_valid = true;
    }

    cJSON *hourly_arr = cJSON_GetObjectItem(root, "hourly");
    if (hourly_arr) {
        hourly_count = 0;
        int count = cJSON_GetArraySize(hourly_arr);
        time_t now = time(NULL);
        for (int i = 0; i < count && hourly_count < MAX_HOURLY; i++) {
            cJSON *item = cJSON_GetArrayItem(hourly_arr, i);
            cJSON *dt = cJSON_GetObjectItem(item, "dt");
            if (!dt) continue;
            time_t ts = (time_t)dt->valueint;
            if (ts < now) continue;
            time_t local_ts = ts + tz_offset;
            struct tm *tm_info = gmtime(&local_ts);
            if (!tm_info) continue;

            hourly_entry_t *h = &hourly[hourly_count];
            h->ts = ts;
            snprintf(h->time_str, sizeof(h->time_str), "%02d:%02d", tm_info->tm_hour, tm_info->tm_min);
            cJSON *t = cJSON_GetObjectItem(item, "temp");
            if (t) h->temp = t->valuedouble;
            cJSON *pop = cJSON_GetObjectItem(item, "pop");
            if (pop) h->pop_pct = (int)(pop->valuedouble * 100);
            cJSON *wa2 = cJSON_GetObjectItem(item, "weather");
            if (wa2 && cJSON_GetArraySize(wa2) > 0) {
                cJSON *w0 = cJSON_GetArrayItem(wa2, 0);
                cJSON *ms = cJSON_GetObjectItem(w0, "main");
                if (ms && ms->valuestring) strncpy(h->desc, ms->valuestring, 15);
            }
            hourly_count++;
        }
    }

    cJSON *daily_arr = cJSON_GetObjectItem(root, "daily");
    if (daily_arr) {
        daily_count = 0;
        int count = cJSON_GetArraySize(daily_arr);
        for (int i = 0; i < count && daily_count < MAX_DAILY; i++) {
            cJSON *item = cJSON_GetArrayItem(daily_arr, i);
            cJSON *dt = cJSON_GetObjectItem(item, "dt");
            if (!dt) continue;

            time_t ts = (time_t)dt->valueint + tz_offset;
            struct tm *tm_info = gmtime(&ts);
            if (!tm_info) continue;

            daily_entry_t *d = &daily[daily_count];
            snprintf(d->day_str, sizeof(d->day_str), "%s", day_names[tm_info->tm_wday]);

            cJSON *temp_obj = cJSON_GetObjectItem(item, "temp");
            if (temp_obj) {
                cJSON *tmin = cJSON_GetObjectItem(temp_obj, "min");
                cJSON *tmax = cJSON_GetObjectItem(temp_obj, "max");
                if (tmin) d->temp_min = tmin->valuedouble;
                if (tmax) d->temp_max = tmax->valuedouble;
            }
            cJSON *hum = cJSON_GetObjectItem(item, "humidity");
            if (hum) d->humidity = hum->valueint;
            cJSON *pop = cJSON_GetObjectItem(item, "pop");
            if (pop) d->pop_pct = (int)(pop->valuedouble * 100);

            cJSON *wa2 = cJSON_GetObjectItem(item, "weather");
            if (wa2 && cJSON_GetArraySize(wa2) > 0) {
                cJSON *w0 = cJSON_GetArrayItem(wa2, 0);
                cJSON *ms = cJSON_GetObjectItem(w0, "main");
                if (ms && ms->valuestring) strncpy(d->desc, ms->valuestring, 23);
            }
            daily_count++;
        }
    }
    cJSON_Delete(root);
}

// --- Cache ---

static void save_cache()
{
    Preferences prefs;
    prefs.begin("weather", false);
    prefs.putBytes("cur", &cur, sizeof(cur));
    prefs.putString("locname", location_name);
    prefs.putBytes("hourly", hourly, sizeof(hourly_entry_t) * hourly_count);
    prefs.putChar("hcnt", hourly_count);
    prefs.putBytes("daily", daily, sizeof(daily_entry_t) * daily_count);
    prefs.putChar("dcnt", daily_count);
    prefs.putULong("ftime", millis());
    prefs.putLong("tz_off", tz_offset);
    prefs.end();
}

static void load_cache()
{
    Preferences prefs;
    prefs.begin("weather", true);
    if (prefs.getBytes("cur", &cur, sizeof(cur)) == sizeof(cur)) {
        String loc = prefs.getString("locname", "");
        strncpy(location_name, loc.c_str(), 63);
        hourly_count = prefs.getChar("hcnt", 0);
        if (hourly_count > MAX_HOURLY) hourly_count = MAX_HOURLY;
        prefs.getBytes("hourly", hourly, sizeof(hourly_entry_t) * hourly_count);
        daily_count = prefs.getChar("dcnt", 0);
        if (daily_count > MAX_DAILY) daily_count = MAX_DAILY;
        prefs.getBytes("daily", daily, sizeof(daily_entry_t) * daily_count);
        last_fetch_time = prefs.getULong("ftime", 0);
        tz_offset = prefs.getLong("tz_off", 0);
        data_valid = true;
    }
    prefs.end();
}

static bool cache_is_fresh()
{
    if (!data_valid || last_fetch_time == 0) return false;
    uint32_t now = millis();
    if (now < last_fetch_time) return false;
    return (now - last_fetch_time) < 3600000UL;
}

// --- Fetch ---

static void fetch_city_name(float lat, float lon)
{
    if (!cfg_has(CFG_OWM_KEY)) return;
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.openweathermap.org/geo/1.0/reverse?lat=%.4f&lon=%.4f&limit=1&appid=%s",
             lat, lon, cfg_get(CFG_OWM_KEY));
    http_response_t resp = http_get(url, 5000);
    if (resp.success) {
        cJSON *arr = cJSON_Parse(resp.body.c_str());
        if (arr && cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0) {
            cJSON *item = cJSON_GetArrayItem(arr, 0);
            cJSON *name = cJSON_GetObjectItem(item, "name");
            if (name && name->valuestring) strncpy(location_name, name->valuestring, 63);
        }
        if (arr) cJSON_Delete(arr);
    }
}

/* The city the user picked, if any. Stored by name plus coordinates so a
 * fetch needs no lookup and the label is right even before the first reply. */
static bool weather_get_choice(char *name, int cap, float *lat, float *lon)
{
    Preferences p;
    p.begin("weather", true);
    String n = p.getString("city", "");
    float la = p.getFloat("clat", 0), lo = p.getFloat("clon", 0);
    p.end();
    if (n.length() == 0) return false;
    snprintf(name, cap, "%s", n.c_str());
    if (lat) *lat = la;
    if (lon) *lon = lo;
    return true;
}

static void weather_set_choice(const city_t *c)
{
    Preferences p;
    p.begin("weather", false);
    if (c) {
        p.putString("city", c->city);
        p.putFloat("clat", city_lat(c));
        p.putFloat("clon", city_lon(c));
    } else {
        p.putString("city", "");        /* back to following GPS */
    }
    p.end();
}

static void close_city_picker(void);
static void city_pick_cb(lv_event_t *e);
static void start_fetch(void);
/* The cache is keyed to nowhere in particular, so a new city has to drop it
 * or start_fetch() would see fresh data and skip the request. */
static void invalidate_cache(void)
{
    data_valid = false;
    last_fetch_time = 0;
}

static void update_city_button(void);
static void invalidate_cache(void);

static void weather_fetch_task(void *param)
{
    float lat = 37.49f, lon = -122.27f;
    const char *loc_source = "fallback";

    /* A city the user picked outranks any GPS fix — it is an explicit choice,
     * and it also means the forecast doesn't move when you do. */
    char chosen[48];
    float clat, clon;
    bool have_choice = weather_get_choice(chosen, sizeof(chosen), &clat, &clon);

    /* Try cached GPS coords */
    Preferences prefs;
    prefs.begin("weather", true);
    float cached_lat = prefs.getFloat("gps_lat", 0);
    float cached_lon = prefs.getFloat("gps_lon", 0);
    prefs.end();
    if (cached_lat != 0 && cached_lon != 0) {
        lat = cached_lat; lon = cached_lon;
        loc_source = "cached GPS";
    }

    /* Try live GPS from factory driver */
    double glat, glng;
    ui_gps_get_coord(&glat, &glng);
    if (glat != 0 && glng != 0) {
        lat = glat; lon = glng;
        loc_source = "GPS";
        Preferences p; p.begin("weather", false);
        p.putFloat("gps_lat", lat); p.putFloat("gps_lon", lon);
        p.end();
    }

    if (have_choice) {
        lat = clat; lon = clon;
        loc_source = "chosen city";
        snprintf(location_name, sizeof(location_name), "%s", chosen);
    }

    Serial.printf("[Weather] Using %s: lat=%.4f lon=%.4f\n", loc_source, lat, lon);

    char url[256];
    snprintf(url, sizeof(url),
             "https://api.openweathermap.org/data/3.0/onecall?lat=%.4f&lon=%.4f&exclude=minutely,alerts&units=metric&appid=%s",
             lat, lon, cfg_get(CFG_OWM_KEY));

    http_response_t resp = http_get(url, 15000);
    if (resp.success) {
        parse_onecall(resp.body.c_str());
        last_fetch_time = millis();
    }

    if (data_valid && location_name[0] == '\0') fetch_city_name(lat, lon);
    if (data_valid) save_cache();
    fetch_task = NULL;
    vTaskDelete(NULL);
}

static void start_fetch()
{
    if (!cfg_has(CFG_OWM_KEY)) {
        if (status_label)
            lv_label_set_text(status_label, "No API key.\nSet owm_api_key in\n/config_keys.ini");
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        if (status_label) lv_label_set_text(status_label, "WiFi not connected");
        return;
    }
    if (fetch_task) return;
    if (cache_is_fresh()) return;
    if (status_label) lv_label_set_text(status_label, "Fetching...");
    xTaskCreatePinnedToCore(weather_fetch_task, "weather", 16384, NULL, 5, &fetch_task, 0);
}

// --- UI update ---

static void update_ui()
{
    if (!data_valid) return;

    if (city_label) lv_label_set_text(city_label, location_name[0] ? location_name : "Unknown");

    if (cur.desc[0] >= 'a' && cur.desc[0] <= 'z') cur.desc[0] -= 32;

    if (icon_img) lv_img_set_src(icon_img, weather_icon_img(cur.icon));

    if (temp_label)
        lv_label_set_text_fmt(temp_label, "%.0f\xC2\xB0" "C  (%.0f\xC2\xB0)",
                              cur.temp, cur.feels_like);

    if (detail_label)
        lv_label_set_text_fmt(detail_label,
                              "%s\nHum:%d%% Wind:%.1fm/s\nPress:%dhPa UV:%d",
                              cur.desc, cur.humidity, cur.wind, cur.pressure, cur.uvi);

    if (status_label) lv_label_set_text(status_label, "");

    /* Hourly forecast table */
    if (hourly_table && hourly_count > 0) {
        char buf[16];
        lv_table_set_row_cnt(hourly_table, hourly_count + 1);
        for (int i = 0; i < hourly_count; i++) {
            int r = i + 1;
            lv_table_set_cell_value(hourly_table, r, 0, hourly[i].time_str);
            snprintf(buf, sizeof(buf), "%.0f\xC2\xB0", hourly[i].temp);
            lv_table_set_cell_value(hourly_table, r, 1, buf);
            lv_table_set_cell_value(hourly_table, r, 2, hourly[i].desc);
            snprintf(buf, sizeof(buf), "%d%%", hourly[i].pop_pct);
            lv_table_set_cell_value(hourly_table, r, 3, buf);
        }
    }

    /* Daily forecast table */
    if (daily_table && daily_count > 0) {
        char buf[16];
        lv_table_set_row_cnt(daily_table, daily_count + 1);
        for (int i = 0; i < daily_count; i++) {
            int r = i + 1;
            lv_table_set_cell_value(daily_table, r, 0, daily[i].day_str);
            snprintf(buf, sizeof(buf), "%.0f/%.0f", daily[i].temp_min, daily[i].temp_max);
            lv_table_set_cell_value(daily_table, r, 1, buf);
            lv_table_set_cell_value(daily_table, r, 2, daily[i].desc);
            snprintf(buf, sizeof(buf), "%d%%", daily[i].pop_pct);
            lv_table_set_cell_value(daily_table, r, 3, buf);
        }
    }
}

static void refresh_cb(lv_timer_t *t)
{
    if (data_valid && !fetch_task) update_ui();
}

// --- Pagination ---


/* ---- city picker overlay ---- */

static void refresh_city_results(void)
{
    if (!city_result_list) return;
    lv_obj_clean(city_result_list);

    /* Always offer the way back to automatic. */
    lv_obj_t *g = lv_list_add_btn(city_result_list, LV_SYMBOL_GPS, "Use GPS location");
    lv_obj_add_event_cb(g, [](lv_event_t *e) {
        weather_set_choice(NULL);
        location_name[0] = '\0';
        invalidate_cache();
        close_city_picker();
        start_fetch();
    }, LV_EVENT_CLICKED, NULL);

    const char *q = city_search_ta ? lv_textarea_get_text(city_search_ta) : "";
    if (!q || !q[0]) return;

    const city_t *hits[24];
    int n = city_search_prefix(q, hits, 24);
    for (int i = 0; i < n; i++) {
        char label[72];
        snprintf(label, sizeof(label), "%s, %s", hits[i]->city, hits[i]->country);
        lv_obj_t *b = lv_list_add_btn(city_result_list, NULL, label);
        lv_obj_set_style_text_font(b, &g_font_cn, LV_PART_MAIN);
        lv_obj_add_event_cb(b, city_pick_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)(hits[i] - city_table));
    }
    if (n == 0) {
        lv_obj_t *b = lv_list_add_btn(city_result_list, NULL, "No match");
        lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void open_city_picker(lv_event_t *e)
{
    if (!city_ovl) return;
    city_picker_open = true;
    lv_textarea_set_text(city_search_ta, "");
    refresh_city_results();
    lv_obj_clear_flag(city_ovl, LV_OBJ_FLAG_HIDDEN);
    ui_disp_full_refr();
}

#define WEATHER_PAGE_COUNT 3
static int weather_page = 0;
static lv_obj_t *pages[WEATHER_PAGE_COUNT] = {};
static lv_obj_t *page_label = NULL;
static bool weather_kbd_active = false;

static const char *page_titles[] = {"Current", "Hourly", "8-Day"};

static void weather_cleanup()
{
    weather_kbd_active = false;
    if (refresh_timer) { lv_timer_del(refresh_timer); refresh_timer = NULL; }
    if (fetch_task) { vTaskDelete(fetch_task); fetch_task = NULL; }
}

static void show_page(int idx)
{
    if (idx < 0 || idx >= WEATHER_PAGE_COUNT) return;
    weather_page = idx;
    for (int i = 0; i < WEATHER_PAGE_COUNT; i++) {
        if (pages[i]) {
            if (i == idx) lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (page_label)
        lv_label_set_text_fmt(page_label, "%s [%d/%d]", page_titles[idx], idx + 1, WEATHER_PAGE_COUNT);
}

// --- Keyboard ---

static void close_city_picker(void)
{
    city_picker_open = false;
    if (city_ovl) lv_obj_add_flag(city_ovl, LV_OBJ_FLAG_HIDDEN);
    update_city_button();
    ui_disp_full_refr();
}

static void city_pick_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= city_count) return;
    const city_t *c = &city_table[idx];
    weather_set_choice(c);
    snprintf(location_name, sizeof(location_name), "%s", c->city);
    if (city_label) lv_label_set_text(city_label, location_name);
    invalidate_cache();
    close_city_picker();
    start_fetch();          /* refetch for the new coordinates */
}

static void update_city_button(void)
{
    if (!city_btn_lbl) return;
    char name[48];
    bool manual = weather_get_choice(name, sizeof(name), NULL, NULL);
    lv_label_set_text(city_btn_lbl, manual ? LV_SYMBOL_EDIT " change"
                                           : LV_SYMBOL_GPS " auto");
}

void weather_keyboard_poll()
{
    if (!weather_kbd_active) return;
    char c;
    if (!keypad_get_val(&c)) return;
    keypad_set_flag();

    if (city_picker_open) {
        if (c == '\n') {
            close_city_picker();
        } else if (c == '\b') {
            const char *t = lv_textarea_get_text(city_search_ta);
            if (!t || !t[0]) close_city_picker();
            else { lv_textarea_del_char(city_search_ta); refresh_city_results(); ui_disp_full_refr(); }
        } else if (c >= ' ') {
            lv_textarea_add_char(city_search_ta, c);
            refresh_city_results();          /* typeahead */
            ui_disp_full_refr();
        }
        return;
    }

    if (c == '\b') {
        if (weather_page > 0) {
            show_page(weather_page - 1);
        } else {
            weather_cleanup();
            scr_mgr_pop(false);
        }
    } else if (c == '\n' || c == ' ') {
        if (weather_page < WEATHER_PAGE_COUNT - 1) {
            show_page(weather_page + 1);
        } else {
            show_page(0);
        }
    }
}

// --- Screen lifecycle ---

static void weather_back_cb(lv_event_t *e)
{
    weather_cleanup();
    scr_mgr_pop(false);
}

static void style_table(lv_obj_t *table)
{
    lv_obj_set_width(table, lv_pct(100));
    lv_obj_clear_flag(table, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_top(table, 1, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(table, 1, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(table, 2, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(table, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_side(table, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
    lv_obj_set_style_border_width(table, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(table, lv_palette_main(LV_PALETTE_GREY), LV_PART_ITEMS);
}

static lv_obj_t *make_page_container(lv_obj_t *parent)
{
    lv_obj_t *pg = lv_obj_create(parent);
    lv_obj_set_size(pg, 236, 260);
    lv_obj_align(pg, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_border_width(pg, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pg, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(pg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pg, 3, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(pg, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
    return pg;
}

static void weather_create(lv_obj_t *parent)
{
    scr_back_btn_create(parent, "Weather", weather_back_cb);

    /* Page indicator at bottom */
    page_label = lv_label_create(parent);
    lv_obj_set_style_text_font(page_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(page_label, LV_ALIGN_BOTTOM_MID, 0, -4);

    /* === Page 0: Current weather === */
    lv_obj_t *p0 = make_page_container(parent);
    pages[0] = p0;

    /* The city name doubles as the picker button — one line of screen for two
     * jobs, and tapping where the city is written is where you'd look. */
    lv_obj_t *city_btn = lv_btn_create(p0);
    lv_obj_set_size(city_btn, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(city_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(city_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(city_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(city_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(city_btn, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(city_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(city_btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_add_event_cb(city_btn, open_city_picker, LV_EVENT_CLICKED, NULL);

    city_label = lv_label_create(city_btn);
    lv_obj_set_style_text_font(city_label, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_label_set_text(city_label, "Loading...");

    city_btn_lbl = lv_label_create(city_btn);
    lv_obj_set_style_text_font(city_btn_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(city_btn_lbl, lv_palette_main(LV_PALETTE_GREY), LV_PART_MAIN);
    lv_label_set_text(city_btn_lbl, "");

    lv_obj_t *row = lv_obj_create(p0);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    icon_img = lv_img_create(row);
    lv_img_set_src(icon_img, &img_w_cloud);

    temp_label = lv_label_create(row);
    lv_obj_set_style_text_font(temp_label, &lv_font_montserrat_26, LV_PART_MAIN);
    lv_label_set_text(temp_label, "--\xC2\xB0" "C");

    detail_label = lv_label_create(p0);
    lv_obj_set_width(detail_label, lv_pct(100));
    lv_obj_set_style_text_font(detail_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(detail_label, "");

    status_label = lv_label_create(p0);
    lv_obj_set_width(status_label, lv_pct(100));
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_palette_main(LV_PALETTE_GREY), LV_PART_MAIN);
    lv_label_set_text(status_label, "Enter/Space: next page");

    /* === Page 1: Hourly forecast === */
    lv_obj_t *p1 = make_page_container(parent);
    pages[1] = p1;

    hourly_table = lv_table_create(p1);
    lv_table_set_col_cnt(hourly_table, 4);
    lv_table_set_col_width(hourly_table, 0, 44);
    lv_table_set_col_width(hourly_table, 1, 44);
    lv_table_set_col_width(hourly_table, 2, 80);
    lv_table_set_col_width(hourly_table, 3, 36);
    style_table(hourly_table);
    const char *hc[] = {"Time", "Temp", "Weather", "Rain"};
    lv_table_set_row_cnt(hourly_table, 1);
    for (int j = 0; j < 4; j++) lv_table_set_cell_value(hourly_table, 0, j, hc[j]);

    /* === Page 2: Daily forecast === */
    lv_obj_t *p2 = make_page_container(parent);
    pages[2] = p2;

    /* Four columns, not five: humidity added little to a daily view and its
     * 32 px left every other column too narrow to read. The 232 px is now
     * spread across the columns people actually scan. */
    daily_table = lv_table_create(p2);
    lv_table_set_col_cnt(daily_table, 4);
    lv_table_set_col_width(daily_table, 0, 44);   /* Day    */
    lv_table_set_col_width(daily_table, 1, 66);   /* Lo/Hi  */
    lv_table_set_col_width(daily_table, 2, 76);   /* Wx     */
    lv_table_set_col_width(daily_table, 3, 44);   /* Rain   */
    style_table(daily_table);
    const char *dc[] = {"Day", "Lo/Hi", "Wx", "Rain"};
    lv_table_set_row_cnt(daily_table, 1);
    for (int j = 0; j < 4; j++) lv_table_set_cell_value(daily_table, 0, j, dc[j]);

    /* === City picker overlay ===
     * Created last so it sits above the pages, and covers the whole app area
     * so a stray tap can't land on the weather behind it. */
    city_ovl = lv_obj_create(parent);
    lv_obj_set_size(city_ovl, 240, 292);
    lv_obj_align(city_ovl, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_border_width(city_ovl, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(city_ovl, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(city_ovl, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(city_ovl, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(city_ovl, 4, LV_PART_MAIN);
    lv_obj_clear_flag(city_ovl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(city_ovl, LV_OBJ_FLAG_HIDDEN);

    city_search_ta = lv_textarea_create(city_ovl);
    lv_obj_set_size(city_search_ta, lv_pct(100), 34);
    lv_textarea_set_one_line(city_search_ta, true);
    lv_textarea_set_placeholder_text(city_search_ta, "Type a city...");
    /* No blinking cursor: on e-ink each blink is a full panel repaint. */
    lv_obj_set_style_anim_time(city_search_ta, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);

    city_result_list = lv_list_create(city_ovl);
    lv_obj_set_size(city_result_list, lv_pct(100), 240);
    lv_obj_set_style_pad_all(city_result_list, 0, LV_PART_MAIN);

    /* Show page 0 */
    weather_page = 0;
    show_page(0);
    update_city_button();

    load_cache();
    if (data_valid) update_ui();
    start_fetch();

    refresh_timer = lv_timer_create(refresh_cb, 2000, NULL);
    weather_kbd_active = true;
}

static void weather_entry(void)
{
    /* Needs the network for the forecast and GPS to know where we are; the
     * last fix is cached in Preferences so a slow lock still shows something. */
    power_acquire(PWR_WIFI);
    power_acquire(PWR_GPS);
    ui_disp_full_refr();
}
static void weather_exit(void)
{
    power_release(PWR_GPS);
    power_release(PWR_WIFI);
    ui_disp_full_refr();
}
static void weather_destroy(void)
{
    weather_cleanup();
    city_label = temp_label = detail_label = status_label = icon_img = NULL;
    hourly_table = daily_table = page_label = NULL;
    city_ovl = city_search_ta = city_result_list = city_btn_lbl = NULL;
    city_picker_open = false;
    for (int i = 0; i < WEATHER_PAGE_COUNT; i++) pages[i] = NULL;
}

scr_lifecycle_t screen_weather = {
    .create = weather_create,
    .entry = weather_entry,
    .exit = weather_exit,
    .destroy = weather_destroy,
};
