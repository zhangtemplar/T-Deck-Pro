/**
 * @file      power_mgr.h
 * @brief     Reference-counted power control for the radios.
 *
 * Every radio used to be switched on in setup() and left on: WiFi associated,
 * GPS receiver running, LoRa enabled and the modem LDO up. Together those
 * dominate the current draw on a 1400 mAh cell, which is why the device only
 * lasted a few hours. Each rail is now off until an app asks for it.
 *
 * Rails are reference counted so overlapping users compose: two screens that
 * both need WiFi keep it up between them, and it only drops when the last one
 * releases. Acquire in a screen's entry(), release in its exit().
 */
#ifndef __POWER_MGR_H__
#define __POWER_MGR_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PWR_WIFI = 0,   /* station mode; also drives modem-sleep while associated */
    PWR_GPS,        /* receiver LDO + the NMEA parser task                    */
    PWR_LORA,       /* SX1262 LDO                                             */
    PWR_MODEM,      /* A7682E LDO (absent on some boards)                     */
    PWR_RAIL_COUNT,
} pwr_rail_t;

/* Put every rail in a known-off state. Call once from setup(), after the pins
 * are configured and before any screen is created. */
void power_mgr_init(void);

/* Turn the rail on if it isn't already, and take a reference. Returns false if
 * the rail could not be brought up (e.g. no WiFi credentials compiled in). */
bool power_acquire(pwr_rail_t rail);

/* Drop a reference. Most rails power down immediately at zero; GPS lingers
 * (see below) so it can be re-acquired without another cold start. */
void power_release(pwr_rail_t rail);

/* How long GPS stays powered after its last release.
 *
 * Every power cycle forces a link re-negotiation (gps_reinit) plus whatever
 * re-acquisition the receiver needs, so holding the rail up briefly means
 * switching between apps, or reopening the GPS screen, doesn't pay that
 * twice. The receiver itself retains its almanac across a supply cut — it
 * survives a flat battery — so this is about the re-init cost, not ephemeris. */
#define PWR_GPS_LINGER_MS (5u * 60u * 1000u)

/* Expire lingering rails whose time is up. Call from loop(). */
void power_mgr_tick(void);

/* Drop every lingering rail right now (used when going idle). */
void power_drop_lingering(void);

/* Power the hardware down while keeping the reference counts, so going idle
 * can switch off even rails a screen is still holding (WiFi in particular).
 * power_resume_all() brings back whatever is still referenced. */
void power_suspend_all(void);

/* True while any rail is still powered. Rails an app holds survive idling, so
 * the idle clock has to stay high enough for whatever is still running. */
bool power_any_rail_on(void);
void power_resume_all(void);

/* Bring WiFi up and block until associated (or timeout). Returns true when
 * connected. Takes a reference either way — release it when done. */
bool power_wifi_connect(uint32_t timeout_ms);

/* Wait for association without changing the reference count. Returns false
 * immediately if nobody is holding the rail up. */
bool power_wifi_wait(uint32_t timeout_ms);

bool        power_is_on(pwr_rail_t rail);
int         power_refcount(pwr_rail_t rail);
const char *power_rail_name(pwr_rail_t rail);

/* One-line summary of what is currently powered, for the serial log. */
void power_log_state(const char *why);

#ifdef __cplusplus
}
#endif

#endif /* __POWER_MGR_H__ */
