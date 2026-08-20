/**
 * @file      power_mgr.cpp
 * @brief     Reference-counted radio power control. See power_mgr.h.
 */
#include "power_mgr.h"
#include <Arduino.h>
#include <WiFi.h>
#include "utilities.h"
#include "peripheral.h"
#include "config_keys.h"
#include "gps_assist.h"

static int s_refs[PWR_RAIL_COUNT];
static bool s_on[PWR_RAIL_COUNT];
/* Non-zero while a rail is powered but unreferenced, waiting to be cut. */
static uint32_t s_off_at[PWR_RAIL_COUNT];
/* True while the hardware is parked for idle but references are still held. */
static bool s_suspended = false;

static const char *k_names[PWR_RAIL_COUNT] = { "wifi", "gps", "lora", "modem" };

const char *power_rail_name(pwr_rail_t rail)
{
    return (rail >= 0 && rail < PWR_RAIL_COUNT) ? k_names[rail] : "?";
}

bool power_is_on(pwr_rail_t rail)   { return (rail < PWR_RAIL_COUNT) && s_on[rail]; }
int  power_refcount(pwr_rail_t rail){ return (rail < PWR_RAIL_COUNT) ? s_refs[rail] : 0; }

void power_log_state(const char *why)
{
    Serial.printf("[PWR] %s:", why ? why : "state");
    for (int i = 0; i < PWR_RAIL_COUNT; i++)
        Serial.printf(" %s=%s(%d)", k_names[i], s_on[i] ? "on" : "off", s_refs[i]);
    Serial.printf("  heap=%uKB psram=%uKB\n",
                  (unsigned)(ESP.getFreeHeap() / 1024),
                  (unsigned)(ESP.getFreePsram() / 1024));
}

/* ---- per-rail power control ---- */

static bool rail_on(pwr_rail_t rail)
{
    switch (rail) {
    case PWR_WIFI:
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        /* Modem sleep: the radio naps between DTIM beacons while associated,
         * which is most of the saving available without disconnecting. */
        WiFi.setSleep(true);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        return true;
#else
        return false;
#endif

    case PWR_GPS:
        digitalWrite(BOARD_GPS_EN, HIGH);
        /* The receiver needs a moment to boot, then the link has to be
         * re-negotiated — it comes back at its power-up defaults, so without
         * this the UART speed and UBX config no longer match and the parser
         * reads nothing at all. */
        delay(300);
        gps_reinit();
        /* Hand the receiver a rough time and position so it doesn't have to
         * search blind — this is what actually shortens time-to-first-fix.
         * Brings WiFi up briefly only if it can contribute something. */
        gps_assist_apply();
        gps_task_resume();
        return true;

    case PWR_LORA:
        digitalWrite(BOARD_LORA_EN, HIGH);
        return true;

    case PWR_MODEM:
        /* Bring the LDO up, then pulse PWRKEY the way setup() does. */
        digitalWrite(BOARD_6609_EN, HIGH);
        delay(100);
        digitalWrite(BOARD_A7682E_PWRKEY, LOW);
        delay(100);
        digitalWrite(BOARD_A7682E_PWRKEY, HIGH);
        delay(1000);
        digitalWrite(BOARD_A7682E_PWRKEY, LOW);
        return true;

    default:
        return false;
    }
}

static void rail_off(pwr_rail_t rail)
{
    switch (rail) {
    case PWR_WIFI:
        WiFi.disconnect(true);       /* also clears the AP config from RAM */
        WiFi.mode(WIFI_OFF);
        break;

    case PWR_GPS:
        /* Stop parsing first so the task isn't reading a dead UART. */
        gps_task_suspend();
        digitalWrite(BOARD_GPS_EN, LOW);
        break;

    case PWR_LORA:
        digitalWrite(BOARD_LORA_EN, LOW);
        break;

    case PWR_MODEM:
        digitalWrite(BOARD_6609_EN, LOW);
        digitalWrite(BOARD_A7682E_PWRKEY, LOW);
        break;

    default:
        break;
    }
}

/* ---- public ---- */

void power_mgr_init(void)
{
    for (int i = 0; i < PWR_RAIL_COUNT; i++) { s_refs[i] = 0; s_on[i] = false; s_off_at[i] = 0; }

    /* Everything down. setup() drives these pins high before we get here, so
     * this is what actually turns the radios off after boot. */
    gps_task_suspend();          /* don't poll a UART with no receiver behind it */
    digitalWrite(BOARD_GPS_EN, LOW);
    digitalWrite(BOARD_LORA_EN, LOW);
    digitalWrite(BOARD_6609_EN, LOW);
    digitalWrite(BOARD_A7682E_PWRKEY, LOW);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    power_log_state("init");
}

bool power_acquire(pwr_rail_t rail)
{
    if (rail >= PWR_RAIL_COUNT) return false;

    if (s_refs[rail] == 0) {
        if (s_on[rail]) {
            /* Still lingering from the last user — reuse it as-is. This is the
             * whole point of the linger: no re-init, no cold start. */
            s_off_at[rail] = 0;
            Serial.printf("[PWR] %s reused (still warm)\n", k_names[rail]);
        } else {
            if (!rail_on(rail)) return false;
            s_on[rail] = true;
            Serial.printf("[PWR] %s on\n", k_names[rail]);
        }
    }
    s_refs[rail]++;
    return true;
}

void power_release(pwr_rail_t rail)
{
    if (rail >= PWR_RAIL_COUNT || s_refs[rail] == 0) return;
    if (--s_refs[rail] != 0) return;

    if (rail == PWR_GPS) {
        /* Skip the power cycle, and the link re-negotiation it forces, if the
         * receiver is wanted again soon; power_mgr_tick() cuts it later. */
        s_off_at[rail] = millis() + PWR_GPS_LINGER_MS;
        if (s_off_at[rail] == 0) s_off_at[rail] = 1;
        Serial.printf("[PWR] %s idle, holding rail %lu s\n",
                      k_names[rail], (unsigned long)(PWR_GPS_LINGER_MS / 1000));
        return;
    }

    rail_off(rail);
    s_on[rail] = false;
    Serial.printf("[PWR] %s off\n", k_names[rail]);
}

void power_mgr_tick(void)
{
    uint32_t now = millis();
    for (int i = 0; i < PWR_RAIL_COUNT; i++) {
        if (!s_on[i] || s_refs[i] > 0 || s_off_at[i] == 0) continue;
        if ((int32_t)(now - s_off_at[i]) < 0) continue;
        rail_off((pwr_rail_t)i);
        s_on[i] = false;
        s_off_at[i] = 0;
        Serial.printf("[PWR] %s off (warm window expired)\n", k_names[i]);
    }
}

void power_suspend_all(void)
{
    if (s_suspended) return;
    s_suspended = true;

    for (int i = 0; i < PWR_RAIL_COUNT; i++) {
        if (!s_on[i]) continue;
        rail_off((pwr_rail_t)i);
        s_on[i] = false;
        s_off_at[i] = 0;
        Serial.printf("[PWR] %s parked (idle, %d ref%s held)\n",
                      k_names[i], s_refs[i], s_refs[i] == 1 ? "" : "s");
    }
}

void power_resume_all(void)
{
    if (!s_suspended) return;
    s_suspended = false;

    for (int i = 0; i < PWR_RAIL_COUNT; i++) {
        if (s_refs[i] == 0 || s_on[i]) continue;
        if (rail_on((pwr_rail_t)i)) {
            s_on[i] = true;
            Serial.printf("[PWR] %s restored\n", k_names[i]);
        }
    }
}

void power_drop_lingering(void)
{
    for (int i = 0; i < PWR_RAIL_COUNT; i++) {
        if (!s_on[i] || s_refs[i] > 0 || s_off_at[i] == 0) continue;
        rail_off((pwr_rail_t)i);
        s_on[i] = false;
        s_off_at[i] = 0;
        Serial.printf("[PWR] %s off (going idle)\n", k_names[i]);
    }
}

bool power_wifi_wait(uint32_t timeout_ms)
{
    if (WiFi.status() == WL_CONNECTED) return true;
    if (!s_on[PWR_WIFI]) return false;          /* nobody asked for the radio */

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeout_ms) delay(50);
    return WiFi.status() == WL_CONNECTED;
}

bool power_wifi_connect(uint32_t timeout_ms)
{
    if (!power_acquire(PWR_WIFI)) return false;
    if (WiFi.status() == WL_CONNECTED) return true;

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeout_ms) {
        delay(50);
    }
    bool ok = (WiFi.status() == WL_CONNECTED);
    Serial.printf("[PWR] wifi %s after %lu ms\n",
                  ok ? "connected" : "TIMEOUT", (unsigned long)(millis() - t0));
    return ok;
}
