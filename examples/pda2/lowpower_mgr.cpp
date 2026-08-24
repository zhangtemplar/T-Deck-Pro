/**
 * @file      lowpower_mgr.cpp
 * @brief     Idle low-power mode. See lowpower_mgr.h.
 */
#include "lowpower_mgr.h"
#include <Arduino.h>
#include "lvgl.h"
#include "utilities.h"
#include "peripheral.h"
#include "pdm_recorder.h"
#include "file_server.h"
#include "ui_gps_enhanced.h"
#include "power_mgr.h"

static uint32_t s_timeout_ms = LOWPOWER_DEFAULT_TIMEOUT_MS;
static uint32_t s_last_activity = 0;
static int      s_inhibit = 0;
static lv_obj_t *s_icon = NULL;
static bool     s_ready = false;
static bool     s_idle = false;

void lowpower_note_activity(void) { s_last_activity = millis(); }

void lowpower_set_timeout(uint32_t ms) { s_timeout_ms = ms; }
uint32_t lowpower_get_timeout(void)    { return s_timeout_ms; }

void lowpower_inhibit(void) { s_inhibit++; }
void lowpower_allow(void)   { if (s_inhibit > 0) s_inhibit--; }

bool lowpower_is_idle(void) { return s_idle; }

void lowpower_init(void)
{
    /* Indicator on the top layer, so it shows over whichever screen is up. */
    s_icon = lv_label_create(lv_layer_top());
    lv_label_set_text(s_icon, LV_SYMBOL_POWER);
    lv_obj_align(s_icon, LV_ALIGN_TOP_RIGHT, -2, 2);
    lv_obj_add_flag(s_icon, LV_OBJ_FLAG_HIDDEN);

    s_last_activity = millis();
    s_ready = true;
    Serial.printf("[LOWPWR] idle after %lu s -> %d MHz\n",
                  (unsigned long)(s_timeout_ms / 1000), LOWPOWER_IDLE_MHZ);
}

/* Work that must keep running with nobody touching the device. */
static bool busy(void)
{
    if (s_inhibit > 0) return true;
    if (pdm_record_is_active() || pdm_play_is_active()) return true;
    if (file_server_is_running()) return true;
    /* A track recording is the one job that runs for hours with no input at
     * all; idling would cut the receiver's power and lose the rest of it. */
    if (gps_track_is_recording()) return true;
    return false;
}

/* Show/hide the indicator and let LVGL push it to the panel. */
static void show_icon(bool on)
{
    if (!s_icon) return;
    if (on) lv_obj_clear_flag(s_icon, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(s_icon, LV_OBJ_FLAG_HIDDEN);

    lv_obj_invalidate(lv_layer_top());
    for (int i = 0; i < 40; i++) {      /* run until rendered and flushed */
        lv_timer_handler();
        delay(10);
    }
}

static void enter_idle(void)
{
    if (s_idle) return;
    s_idle = true;

    show_icon(true);

    /* Park the radios nothing is holding. Anything a screen still has a
     * reference to stays powered — see power_suspend_all(). */
    power_suspend_all();

    /* 40 MHz is only safe once every radio is off; WiFi needs 80. Rails an app
     * still holds are no longer parked, so ask rather than assume. */
    int mhz = power_any_rail_on() ? LOWPOWER_RADIO_MHZ : LOWPOWER_IDLE_MHZ;
    setCpuFrequencyMhz(mhz);
    Serial.printf("[LOWPWR] idle: cpu %d MHz\n", mhz);
    power_log_state("idle");
}

static void exit_idle(void)
{
    if (!s_idle) return;

    /* Clock first, so the redraw and any radio re-init run at full speed. */
    setCpuFrequencyMhz(LOWPOWER_ACTIVE_MHZ);
    s_idle = false;
    power_resume_all();
    Serial.printf("[LOWPWR] active: cpu %d MHz\n", LOWPOWER_ACTIVE_MHZ);

    show_icon(false);
    lowpower_note_activity();
}

void lowpower_poll(void)
{
    if (!s_ready) return;

    /* Keys are timestamped by the keypad driver; touch is fed in from the
     * LVGL read callback. */
    uint32_t k = keypad_last_activity_ms();
    if (k > s_last_activity) s_last_activity = k;

    bool active = (millis() - s_last_activity) < s_timeout_ms;

    if (s_timeout_ms == 0 || busy() || active) {
        exit_idle();                 /* no-op when already running full speed */
        if (busy()) lowpower_note_activity();
        return;
    }

    enter_idle();
}
