/**
 * @file      gps_assist.cpp
 * @brief     Assisted GNSS via UBX-MGA-INI. See gps_assist.h.
 */
#include "gps_assist.h"
#include <string.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include "utilities.h"
#include "peripheral.h"
#include "power_mgr.h"
#include "config_keys.h"
#include "app_config.h"
#include "http_utils.h"
#endif

/* ---- UBX framing ---------------------------------------------------------
 * B5 62 | class id | len(2, LE) | payload | ckA ckB
 * Checksum is 8-bit Fletcher over class..payload inclusive. */

#define UBX_CLASS_MGA 0x13
#define UBX_ID_MGA_INI 0x40

static int ubx_wrap(uint8_t *buf, uint8_t cls, uint8_t id,
                    const uint8_t *payload, uint16_t len)
{
    buf[0] = 0xB5;
    buf[1] = 0x62;
    buf[2] = cls;
    buf[3] = id;
    buf[4] = (uint8_t)(len & 0xFF);
    buf[5] = (uint8_t)(len >> 8);
    memcpy(buf + 6, payload, len);

    uint8_t a = 0, b = 0;
    for (uint16_t i = 2; i < 6 + len; i++) { a = (uint8_t)(a + buf[i]); b = (uint8_t)(b + a); }
    buf[6 + len] = a;
    buf[7 + len] = b;
    return 8 + len;
}

static void put_u16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static void put_i32(uint8_t *p, int32_t v) { put_u32(p, (uint32_t)v); }

int gps_assist_build_time(uint8_t *buf, int year, int mon, int day,
                          int hour, int min, int sec, uint16_t acc_s)
{
    uint8_t pl[24];
    memset(pl, 0, sizeof(pl));
    pl[0] = 0x10;                 /* type: INI-TIME_UTC                     */
    pl[1] = 0x00;                 /* version                                */
    pl[2] = 0x00;                 /* ref: no external time pulse            */
    pl[3] = (uint8_t)0x80;        /* leapSecs = -128, i.e. unknown          */
    put_u16(pl + 4, (uint16_t)year);
    pl[6]  = (uint8_t)mon;
    pl[7]  = (uint8_t)day;
    pl[8]  = (uint8_t)hour;
    pl[9]  = (uint8_t)min;
    pl[10] = (uint8_t)sec;
    /* pl[11] reserved, pl[12..15] ns = 0 */
    put_u16(pl + 16, acc_s);      /* tAccS                                   */
    /* pl[18..19] reserved, pl[20..23] tAccNs = 0 */
    return ubx_wrap(buf, UBX_CLASS_MGA, UBX_ID_MGA_INI, pl, sizeof(pl));
}

int gps_assist_build_pos(uint8_t *buf, double lat, double lon,
                         int32_t alt_cm, uint32_t acc_cm)
{
    uint8_t pl[20];
    memset(pl, 0, sizeof(pl));
    pl[0] = 0x01;                 /* type: INI-POS_LLH                       */
    pl[1] = 0x00;                 /* version                                 */
    /* pl[2..3] reserved */
    put_i32(pl + 4,  (int32_t)(lat * 1e7));
    put_i32(pl + 8,  (int32_t)(lon * 1e7));
    put_i32(pl + 12, alt_cm);
    put_u32(pl + 16, acc_cm);
    return ubx_wrap(buf, UBX_CLASS_MGA, UBX_ID_MGA_INI, pl, sizeof(pl));
}

#ifdef ARDUINO

/* Coarse aiding: claim only what we can stand behind. Time over a UART has
 * milliseconds of jitter and the cached position may be a city away. */
#define ASSIST_TIME_ACC_S   2u
#define ASSIST_POS_ACC_CM   5000000u      /* 50 km */

static bool clock_is_set(struct tm *out)
{
    time_t now;
    time(&now);
    localtime_r(&now, out);
    return (out->tm_year + 1900) >= 2024;
}

/* Full ephemeris from u-blox AssistNow Online. The response is already a
 * stream of UBX-MGA messages, so it goes straight at the receiver. */
static void assistnow_fetch(void)
{
    if (!cfg_has(CFG_ASSISTNOW_TOKEN)) return;

    char url[256];
    snprintf(url, sizeof(url),
             "https://online-live1.services.u-blox.com/GetOnlineData.ashx"
             "?token=%s&gnss=gps,glo&datatype=eph,alm,aux",
             cfg_get(CFG_ASSISTNOW_TOKEN));

    http_response_t r = http_get(url, 10000);
    if (!r.success || r.body.empty()) {
        Serial.printf("[AGPS] AssistNow fetch failed: %s\n", r.body.c_str());
        return;
    }
    SerialGPS.write((const uint8_t *)r.body.data(), r.body.size());
    SerialGPS.flush();
    Serial.printf("[AGPS] AssistNow: injected %u bytes\n", (unsigned)r.body.size());
}

void gps_assist_apply(void)
{
    struct tm tm;
    bool have_clock = clock_is_set(&tm);

    /* WiFi earns its power here only if it can supply something we lack. */
    bool want_wifi = !have_clock || cfg_has(CFG_ASSISTNOW_TOKEN);
    bool wifi_held = false;

    if (want_wifi) {
        wifi_held = power_wifi_connect(8000);
        if (wifi_held && !have_clock) {
            struct tm t2;
            if (getLocalTime(&t2, 5000)) { tm = t2; have_clock = true; }
        }
    }

    uint8_t frame[40];

    if (have_clock) {
        /* struct tm is local time; UTC is what the receiver wants. */
        time_t now;
        time(&now);
        struct tm utc;
        gmtime_r(&now, &utc);
        int n = gps_assist_build_time(frame, utc.tm_year + 1900, utc.tm_mon + 1,
                                      utc.tm_mday, utc.tm_hour, utc.tm_min,
                                      utc.tm_sec, ASSIST_TIME_ACC_S);
        SerialGPS.write(frame, n);
        Serial.printf("[AGPS] time aiding %04d-%02d-%02d %02d:%02d:%02dZ (+/-%us)\n",
                      utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                      utc.tm_hour, utc.tm_min, utc.tm_sec, ASSIST_TIME_ACC_S);
    } else {
        Serial.println("[AGPS] no clock available, skipping time aiding");
    }

    /* Last known position, shared with the weather app. */
    Preferences p;
    p.begin("weather", true);
    float lat = p.getFloat("gps_lat", 0);
    float lon = p.getFloat("gps_lon", 0);
    p.end();

    if (lat != 0 || lon != 0) {
        int n = gps_assist_build_pos(frame, lat, lon, 0, ASSIST_POS_ACC_CM);
        SerialGPS.write(frame, n);
        Serial.printf("[AGPS] position aiding %.4f,%.4f (+/-%u km)\n",
                      lat, lon, ASSIST_POS_ACC_CM / 100000u);
    } else {
        Serial.println("[AGPS] no cached position, skipping position aiding");
    }

    if (wifi_held) assistnow_fetch();

    SerialGPS.flush();
    if (want_wifi) power_release(PWR_WIFI);
}

#endif /* ARDUINO */
