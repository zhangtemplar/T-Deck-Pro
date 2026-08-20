/**
 * @file      ui_calendar.cpp
 * @brief     Calendar app with holidays (Calendarific API + computed fallback + jieqi).
 *            Two pages: calendar view + holiday list.
 */
#include "Arduino.h"
#include "ui_deckpro.h"
#include "ui_deckpro_port.h"
#include "power_mgr.h"
#include "lunar_calendar.h"
#include <time.h>
#include "cjk_font.h"

static lv_obj_t *calendar_obj = NULL;
static lv_obj_t *holiday_label = NULL;
static lv_obj_t *page_cal = NULL;
static lv_obj_t *page_holidays = NULL;
static lv_obj_t *page_indicator = NULL;
static lv_calendar_date_t highlighted_days_buf[31];

static int current_year = 2026;
static int current_month = 4;
static int cal_page = 0;
static bool cal_kbd_active = false;

static int hol_days[31];
static const char *hol_names[31];
static int hol_count = 0;

static lv_timer_t *poll_timer = NULL;
static TaskHandle_t fetch_task_handle = NULL;
static volatile bool fetch_done = false;
static int fetch_year = 0, fetch_month = 0;

static void show_cal_page(int pg)
{
    cal_page = pg;
    if (pg == 0) {
        if (page_cal) lv_obj_clear_flag(page_cal, LV_OBJ_FLAG_HIDDEN);
        if (page_holidays) lv_obj_add_flag(page_holidays, LV_OBJ_FLAG_HIDDEN);
        if (page_indicator) lv_label_set_text(page_indicator, "Calendar [1/2]  Enter:holidays");
    } else {
        if (page_cal) lv_obj_add_flag(page_cal, LV_OBJ_FLAG_HIDDEN);
        if (page_holidays) lv_obj_clear_flag(page_holidays, LV_OBJ_FLAG_HIDDEN);
        if (page_indicator) lv_label_set_text(page_indicator, "Holidays [2/2]  A/D:month  Bksp:back");
    }
}

static void refresh_holiday_ui(int year, int month)
{
    if (holiday_label) {
        if (hol_count > 0) {
            char buf[1024];
            int pos = snprintf(buf, sizeof(buf), "%d/%d  (A:prev D:next)\n", year, month);
            for (int i = 0; i < hol_count && pos < (int)sizeof(buf) - 1; i++) {
                pos += snprintf(buf + pos, sizeof(buf) - pos, " %2d: %s\n",
                                hol_days[i], hol_names[i]);
            }
            lv_label_set_text(holiday_label, buf);
        } else {
            lv_label_set_text_fmt(holiday_label, "%d/%d  (A:prev D:next)\nNo holidays", year, month);
        }
    }
}

static void load_holidays_for_month(int year, int month)
{
    hol_count = get_month_holidays(year, month, hol_days, hol_names);
    Serial.printf("[Cal] holidays for %d/%d: %d entries\n", year, month, hol_count);
    refresh_holiday_ui(year, month);
}

/* Background API fetch */
static void fetch_task_func(void *param)
{
    Serial.printf("[Cal] API fetch %d/%d...\n", fetch_year, fetch_month);
    holidays_fetch_api(fetch_year, fetch_month);
    Serial.printf("[Cal] API fetch done\n");
    fetch_done = true;
    fetch_task_handle = NULL;
    vTaskDelete(NULL);
}

static void start_api_fetch(int year, int month)
{
    if (fetch_task_handle) { vTaskDelete(fetch_task_handle); fetch_task_handle = NULL; }
    fetch_year = year;
    fetch_month = month;
    fetch_done = false;
    xTaskCreatePinnedToCore(fetch_task_func, "hol_fetch", 8192, NULL, 5, &fetch_task_handle, 0);
}

static int hol_year = 0, hol_month = 0;

static void navigate_holidays(int delta)
{
    hol_month += delta;
    if (hol_month > 12) { hol_month = 1; hol_year++; }
    if (hol_month < 1)  { hol_month = 12; hol_year--; }
    load_holidays_for_month(hol_year, hol_month);
    start_api_fetch(hol_year, hol_month);
}

static void poll_timer_cb(lv_timer_t *t)
{
    if (fetch_done) {
        fetch_done = false;
        Serial.printf("[Cal] API data ready\n");
        load_holidays_for_month(fetch_year, fetch_month);
    } else if (fetch_task_handle && holiday_label) {
        const char *txt = lv_label_get_text(holiday_label);
        if (txt && !strstr(txt, "Fetching"))
            lv_label_set_text_fmt(holiday_label, "%s\n[Fetching...]", txt);
    }
}

/* Keyboard navigation */
void calendar_keyboard_poll()
{
    if (!cal_kbd_active) return;
    char c;
    if (!keypad_get_val(&c)) return;
    keypad_set_flag();

    if (c == '\b') {
        if (cal_page > 0) {
            show_cal_page(cal_page - 1);
        } else {
            cal_kbd_active = false;
            scr_mgr_pop(false);
        }
    } else if (c == '\n' || c == ' ') {
        show_cal_page(cal_page == 0 ? 1 : 0);
    } else if (c == 'a') {
        if (cal_page == 1) navigate_holidays(-1);
    } else if (c == 'd') {
        if (cal_page == 1) navigate_holidays(1);
    }
}

/* Screen lifecycle */
static void cal_back_cb(lv_event_t *e)
{
    cal_kbd_active = false;
    scr_mgr_pop(false);
}

static void cal_create(lv_obj_t *parent)
{
    scr_back_btn_create(parent, "Calendar", cal_back_cb);

    struct tm timeinfo;
    time_t now = time(NULL);
    localtime_r(&now, &timeinfo);
    int year = timeinfo.tm_year + 1900;
    int month = timeinfo.tm_mon + 1;
    int day = timeinfo.tm_mday;
    if (year < 2020 || year > 2099) {
        uint16_t gy; uint8_t gm, gd;
        ui_gps_get_data(&gy, &gm, &gd);
        if (gy >= 2020 && gy <= 2099) { year = gy; month = gm; day = gd; }
    }
    Serial.printf("[Cal] init date: %d/%d/%d\n", year, month, day);
    current_year = year;
    current_month = month;

    page_indicator = lv_label_create(parent);
    lv_obj_set_style_text_font(page_indicator, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(page_indicator, LV_ALIGN_BOTTOM_MID, 0, -4);

    /* Page 0: Calendar */
    page_cal = lv_obj_create(parent);
    lv_obj_set_size(page_cal, 236, 264);
    lv_obj_align(page_cal, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_border_width(page_cal, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(page_cal, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(page_cal, 0, LV_PART_MAIN);
    lv_obj_clear_flag(page_cal, LV_OBJ_FLAG_SCROLLABLE);

    calendar_obj = lv_calendar_create(page_cal);
    lv_obj_set_size(calendar_obj, 234, 260);
    lv_obj_align(calendar_obj, LV_ALIGN_TOP_MID, 0, 0);
    lv_calendar_set_today_date(calendar_obj, year, month, day);
    lv_calendar_set_showed_date(calendar_obj, year, month);
    lv_calendar_header_arrow_create(calendar_obj);

    /* Page 1: Holiday list */
    page_holidays = lv_obj_create(parent);
    lv_obj_set_size(page_holidays, 236, 264);
    lv_obj_align(page_holidays, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_border_width(page_holidays, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(page_holidays, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(page_holidays, 4, LV_PART_MAIN);
    lv_obj_clear_flag(page_holidays, LV_OBJ_FLAG_SCROLLABLE);

    holiday_label = lv_label_create(page_holidays);
    lv_obj_set_width(holiday_label, lv_pct(100));
    /* CJK-capable font so lunar/jieqi/holiday names in Chinese render.
     * Coverage is limited to the font's built-in glyph set until it is
     * regenerated with the full character set (see build docs). */
    lv_obj_set_style_text_font(holiday_label, &g_font_cn, LV_PART_MAIN);
    lv_label_set_long_mode(holiday_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(holiday_label, "Loading...");

    hol_year = year;
    hol_month = month;
    load_holidays_for_month(year, month);
    start_api_fetch(year, month);
    poll_timer = lv_timer_create(poll_timer_cb, 500, NULL);
    show_cal_page(0);
    cal_kbd_active = true;
}

static void cal_entry(void) { power_acquire(PWR_WIFI); ui_disp_full_refr(); }
static void cal_exit(void) { power_release(PWR_WIFI); ui_disp_full_refr(); }
static void cal_destroy(void)
{
    cal_kbd_active = false;
    if (fetch_task_handle) { vTaskDelete(fetch_task_handle); fetch_task_handle = NULL; }
    if (poll_timer) { lv_timer_del(poll_timer); poll_timer = NULL; }
    fetch_done = false;
    calendar_obj = holiday_label = page_cal = page_holidays = page_indicator = NULL;
}

scr_lifecycle_t screen_calendar = {
    .create = cal_create,
    .entry = cal_entry,
    .exit = cal_exit,
    .destroy = cal_destroy,
};
