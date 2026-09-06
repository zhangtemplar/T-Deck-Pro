/**
 * @file      lunar_calendar.cpp
 * @brief     Holidays: Calendarific API (primary) + computed US/Chinese (fallback) + 24 jieqi.
 */
#include "lunar_calendar.h"
#include "config_keys.h"
#include "app_config.h"
#include <string.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <pgmspace.h>
#include <Preferences.h>
#include <WiFi.h>
#include <cJSON.h>
#include "http_utils.h"
#else
#define PROGMEM
#endif

// ---- Day of week (Tomohiko Sakamoto) ----

int day_of_week(int y, int m, int d)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

// ---- Computed US Federal Holidays ----

static int nth_weekday(int year, int month, int weekday, int n)
{
    int first = day_of_week(year, month, 1);
    return 1 + ((weekday - first + 7) % 7) + (n - 1) * 7;
}

static int last_weekday(int year, int month, int weekday)
{
    static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int dim = mdays[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) dim = 29;
    int last_dow = day_of_week(year, month, dim);
    return dim - ((last_dow - weekday + 7) % 7);
}

const char *get_us_holiday(int year, int month, int day)
{
    switch (month) {
    case 1:
        if (day == 1) return "New Year's Day";
        if (day == nth_weekday(year, 1, 1, 3)) return "MLK Day";
        break;
    case 2:
        if (day == nth_weekday(year, 2, 1, 3)) return "Presidents' Day";
        break;
    case 5:
        if (day == last_weekday(year, 5, 1)) return "Memorial Day";
        break;
    case 6:
        if (day == 19) return "Juneteenth";
        break;
    case 7:
        if (day == 4) return "Independence Day";
        break;
    case 9:
        if (day == nth_weekday(year, 9, 1, 1)) return "Labor Day";
        break;
    case 10:
        if (day == nth_weekday(year, 10, 1, 2)) return "Columbus Day";
        break;
    case 11:
        if (day == 11) return "Veterans Day";
        if (day == nth_weekday(year, 11, 4, 4)) return "Thanksgiving";
        break;
    case 12:
        if (day == 25) return "Christmas Day";
        break;
    }
    return NULL;
}

// ---- Computed Chinese Holidays (bilingual, pre-computed) ----

typedef struct {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    const char *name;
} holiday_entry_t;

static const holiday_entry_t chinese_holidays[] PROGMEM = {
    {2025, 1, 29, "Spring Festival 春节"}, {2026, 2, 17, "Spring Festival 春节"},
    {2027, 2, 6,  "Spring Festival 春节"}, {2028, 1, 26, "Spring Festival 春节"},
    {2025, 2, 12, "Lantern 元宵"},  {2026, 3, 3,  "Lantern 元宵"},
    {2027, 2, 20, "Lantern 元宵"},  {2028, 2, 9,  "Lantern 元宵"},
    {2025, 5, 31, "Dragon Boat 端午"}, {2026, 6, 19, "Dragon Boat 端午"},
    {2027, 6, 9,  "Dragon Boat 端午"}, {2028, 5, 28, "Dragon Boat 端午"},
    {2025, 10, 6, "Mid-Autumn 中秋"}, {2026, 9, 25, "Mid-Autumn 中秋"},
    {2027, 9, 15, "Mid-Autumn 中秋"}, {2028, 10, 3, "Mid-Autumn 中秋"},
    {2025, 10, 29,"Double Ninth 重阳"},{2026, 10, 18,"Double Ninth 重阳"},
    {2027, 10, 8, "Double Ninth 重阳"},{2028, 10, 26,"Double Ninth 重阳"},
};
#define CHINESE_HOLIDAY_COUNT (sizeof(chinese_holidays) / sizeof(chinese_holidays[0]))

const char *get_chinese_holiday(int year, int month, int day)
{
    for (int i = 0; i < (int)CHINESE_HOLIDAY_COUNT; i++) {
        if (chinese_holidays[i].year == year && chinese_holidays[i].month == month &&
            chinese_holidays[i].day == day)
            return chinese_holidays[i].name;
    }
    return NULL;
}

// ---- 24 Jieqi (pre-computed 2025-2028) ----

static const holiday_entry_t jieqi_table[] PROGMEM = {
    {2025,1,5,"Xiaohan 小寒"},{2025,1,20,"Dahan 大寒"},{2025,2,3,"Lichun 立春"},
    {2025,2,18,"Yushui 雨水"},{2025,3,5,"Jingzhe 惊蛰"},{2025,3,20,"Chunfen 春分"},
    {2025,4,4,"Qingming 清明"},{2025,4,20,"Guyu 谷雨"},{2025,5,5,"Lixia 立夏"},
    {2025,5,21,"Xiaoman 小满"},{2025,6,5,"Mangzhong 芒种"},{2025,6,21,"Xiazhi 夏至"},
    {2025,7,7,"Xiaoshu 小暑"},{2025,7,22,"Dashu 大暑"},{2025,8,7,"Liqiu 立秋"},
    {2025,8,23,"Chushu 处暑"},{2025,9,7,"Bailu 白露"},{2025,9,22,"Qiufen 秋分"},
    {2025,10,8,"Hanlu 寒露"},{2025,10,23,"Shuangjiang 霜降"},{2025,11,7,"Lidong 立冬"},
    {2025,11,22,"Xiaoxue 小雪"},{2025,12,7,"Daxue 大雪"},{2025,12,21,"Dongzhi 冬至"},
    {2026,1,5,"Xiaohan 小寒"},{2026,1,20,"Dahan 大寒"},{2026,2,4,"Lichun 立春"},
    {2026,2,18,"Yushui 雨水"},{2026,3,5,"Jingzhe 惊蛰"},{2026,3,20,"Chunfen 春分"},
    {2026,4,5,"Qingming 清明"},{2026,4,20,"Guyu 谷雨"},{2026,5,5,"Lixia 立夏"},
    {2026,5,21,"Xiaoman 小满"},{2026,6,5,"Mangzhong 芒种"},{2026,6,21,"Xiazhi 夏至"},
    {2026,7,7,"Xiaoshu 小暑"},{2026,7,22,"Dashu 大暑"},{2026,8,7,"Liqiu 立秋"},
    {2026,8,23,"Chushu 处暑"},{2026,9,7,"Bailu 白露"},{2026,9,23,"Qiufen 秋分"},
    {2026,10,8,"Hanlu 寒露"},{2026,10,23,"Shuangjiang 霜降"},{2026,11,7,"Lidong 立冬"},
    {2026,11,22,"Xiaoxue 小雪"},{2026,12,7,"Daxue 大雪"},{2026,12,22,"Dongzhi 冬至"},
    {2027,1,5,"Xiaohan 小寒"},{2027,1,20,"Dahan 大寒"},{2027,2,4,"Lichun 立春"},
    {2027,2,18,"Yushui 雨水"},{2027,3,5,"Jingzhe 惊蛰"},{2027,3,20,"Chunfen 春分"},
    {2027,4,5,"Qingming 清明"},{2027,4,20,"Guyu 谷雨"},{2027,5,5,"Lixia 立夏"},
    {2027,5,21,"Xiaoman 小满"},{2027,6,5,"Mangzhong 芒种"},{2027,6,21,"Xiazhi 夏至"},
    {2027,7,7,"Xiaoshu 小暑"},{2027,7,23,"Dashu 大暑"},{2027,8,7,"Liqiu 立秋"},
    {2027,8,23,"Chushu 处暑"},{2027,9,7,"Bailu 白露"},{2027,9,23,"Qiufen 秋分"},
    {2027,10,8,"Hanlu 寒露"},{2027,10,23,"Shuangjiang 霜降"},{2027,11,7,"Lidong 立冬"},
    {2027,11,22,"Xiaoxue 小雪"},{2027,12,7,"Daxue 大雪"},{2027,12,22,"Dongzhi 冬至"},
};
#define JIEQI_COUNT (sizeof(jieqi_table) / sizeof(jieqi_table[0]))

static const char *lookup_table(const holiday_entry_t *tbl, int len, int y, int m, int d)
{
    for (int i = 0; i < len; i++)
        if (tbl[i].year == y && tbl[i].month == m && tbl[i].day == d) return tbl[i].name;
    return NULL;
}

const char *get_holiday_name(int year, int month, int day)
{
    const char *n = get_us_holiday(year, month, day);
    if (n) return n;
    n = get_chinese_holiday(year, month, day);
    if (n) return n;
    return lookup_table(jieqi_table, JIEQI_COUNT, year, month, day);
}

// ============================================================
// Calendarific API integration (Arduino only)
// ============================================================

#ifdef ARDUINO

#define API_CACHE_MAX 64

struct api_holiday_t {
    uint8_t month;
    uint8_t day;
    char name[48];
};

static api_holiday_t api_cache[API_CACHE_MAX];
static int api_cache_count = 0;
static int api_cached_year = 0;
static int api_cached_month = 0;
static bool api_fetched = false;

static void load_api_cache(int year, int month)
{
    Preferences prefs;
    prefs.begin("holidays", true);
    int cached_yr = prefs.getInt("year", 0);
    int cached_mo = prefs.getInt("month", 0);
    if (cached_yr == year && cached_mo == month) {
        api_cache_count = prefs.getInt("count", 0);
        if (api_cache_count > API_CACHE_MAX) api_cache_count = API_CACHE_MAX;
        prefs.getBytes("data", api_cache, sizeof(api_holiday_t) * api_cache_count);
        api_cached_year = year;
        api_cached_month = month;
        api_fetched = true;
        Serial.printf("[Holiday] Loaded %d cached entries for %d/%d\n", api_cache_count, year, month);
    }
    prefs.end();
}

static void save_api_cache(int year, int month)
{
    Preferences prefs;
    prefs.begin("holidays", false);
    prefs.putInt("year", year);
    prefs.putInt("month", month);
    prefs.putInt("count", api_cache_count);
    prefs.putBytes("data", api_cache, sizeof(api_holiday_t) * api_cache_count);
    prefs.end();
}

static void parse_calendarific(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;

    cJSON *resp = cJSON_GetObjectItem(root, "response");
    if (!resp) { cJSON_Delete(root); return; }

    cJSON *holidays = cJSON_GetObjectItem(resp, "holidays");
    if (!holidays || !cJSON_IsArray(holidays)) { cJSON_Delete(root); return; }

    int count = cJSON_GetArraySize(holidays);
    for (int i = 0; i < count && api_cache_count < API_CACHE_MAX; i++) {
        cJSON *item = cJSON_GetArrayItem(holidays, i);
        cJSON *name_obj = cJSON_GetObjectItem(item, "name");
        cJSON *date_obj = cJSON_GetObjectItem(item, "date");
        if (!name_obj || !date_obj) continue;

        cJSON *dt = cJSON_GetObjectItem(date_obj, "datetime");
        if (!dt) continue;

        cJSON *m = cJSON_GetObjectItem(dt, "month");
        cJSON *d = cJSON_GetObjectItem(dt, "day");
        if (!m || !d) continue;

        api_holiday_t *h = &api_cache[api_cache_count];
        h->month = m->valueint;
        h->day = d->valueint;
        strncpy(h->name, name_obj->valuestring, 47);
        h->name[47] = '\0';
        api_cache_count++;
    }

    cJSON_Delete(root);
}

void holidays_fetch_api(int year, int month)
{
    if (!cfg_has(CFG_CALENDARIFIC_KEY)) return;
    if (api_fetched && api_cached_year == year && api_cached_month == month) return;

    load_api_cache(year, month);
    if (api_fetched && api_cached_year == year && api_cached_month == month) return;

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[Holiday] No WiFi, using computed fallback");
        return;
    }

    api_cache_count = 0;

    /* Comma-separated in the .ini ("US,CN") so it can be edited on the card;
     * the compile-time CALENDAR_COUNTRIES stays a braced list of quoted
     * strings, so it is flattened to the same form here. */
    char clist[96];
#ifdef CALENDAR_COUNTRIES
    {
        const char *tmp[] = { CALENDAR_COUNTRIES };
        size_t pos = 0;
        for (size_t i = 0; i < sizeof(tmp) / sizeof(tmp[0]); i++)
            pos += snprintf(clist + pos, sizeof(clist) - pos, "%s%s", i ? "," : "", tmp[i]);
    }
#else
    clist[0] = '\0';
#endif
    if (cfg_source(CFG_CALENDAR_COUNTRY)[0] == 's')      /* "sd" wins */
        snprintf(clist, sizeof(clist), "%s", cfg_get(CFG_CALENDAR_COUNTRY));
    if (!clist[0]) snprintf(clist, sizeof(clist), "US");

    const char *countries[8];
    int num_countries = 0;
    for (char *tok = strtok(clist, ", "); tok && num_countries < 8; tok = strtok(NULL, ", "))
        countries[num_countries++] = tok;

    for (int c = 0; c < num_countries && api_cache_count < API_CACHE_MAX; c++) {
        char url[320];
        snprintf(url, sizeof(url),
                 "https://calendarific.com/api/v2/holidays?api_key=%s&country=%s&year=%d&month=%d&type=national",
                 cfg_get(CFG_CALENDARIFIC_KEY), countries[c], year, month);

        Serial.printf("[Holiday] Fetching %s/%d/%d...\n", countries[c], year, month);
        http_response_t resp = http_get(url, 15000);
        if (resp.success) {
            parse_calendarific(resp.body.c_str());
            Serial.printf("[Holiday] %s: total %d entries so far\n", countries[c], api_cache_count);
        } else {
            Serial.printf("[Holiday] %s fetch failed: %s\n", countries[c], resp.body.c_str());
        }
    }

    if (api_cache_count > 0) {
        api_cached_year = year;
        api_cached_month = month;
        api_fetched = true;
        save_api_cache(year, month);
    }
}

static int get_api_holidays(int year, int month, int *days, const char **names, int max_out)
{
    if (!api_fetched || api_cached_year != year || api_cached_month != month) return 0;
    int count = 0;
    for (int i = 0; i < api_cache_count && count < max_out; i++) {
        if (api_cache[i].month == month) {
            days[count] = api_cache[i].day;
            names[count] = api_cache[i].name;
            count++;
        }
    }
    return count;
}

#endif // ARDUINO

// ---- get_month_holidays: API first, then computed fallback + jieqi ----

int get_month_holidays(int year, int month, int *days, const char **names)
{
    int count = 0;

#ifdef ARDUINO
    count = get_api_holidays(year, month, days, names, 31);
    if (count > 0) {
        /* API succeeded — use API results + jieqi only */
        for (int i = 0; i < (int)JIEQI_COUNT && count < 31; i++) {
            if (jieqi_table[i].year == year && jieqi_table[i].month == month) {
                bool dup = false;
                for (int j = 0; j < count; j++) {
                    if (days[j] == (int)jieqi_table[i].day) { dup = true; break; }
                }
                if (!dup) {
                    days[count] = jieqi_table[i].day;
                    names[count] = jieqi_table[i].name;
                    count++;
                }
            }
        }
        return count;
    }
#endif

    /* Fallback: computed US + Chinese + Jieqi */
    static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int dim = mdays[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) dim = 29;

    for (int d = 1; d <= dim && count < 31; d++) {
        const char *us = get_us_holiday(year, month, d);
        if (us && count < 31) { days[count] = d; names[count] = us; count++; }
        const char *cn = get_chinese_holiday(year, month, d);
        if (cn && count < 31) { days[count] = d; names[count] = cn; count++; }
        const char *jq = lookup_table(jieqi_table, JIEQI_COUNT, year, month, d);
        if (jq && !cn && count < 31) { days[count] = d; names[count] = jq; count++; }
    }
    return count;
}
