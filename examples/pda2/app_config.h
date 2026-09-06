/**
 * @file      app_config.h
 * @brief     Settings from the SD card, falling back to the compile-time keys.
 *
 * Credentials used to be reachable only through config_keys.h, which meant a
 * rebuild and a reflash to change a WiFi password. This reads the same values
 * from /config_keys.ini on the card, so a device can be handed to someone else,
 * moved to another network, or given an API key without a toolchain.
 *
 * Precedence is card over firmware: whatever is in the .ini wins, and anything
 * it does not mention keeps the value compiled in. That way an existing build
 * behaves exactly as before until a file appears.
 *
 * File format is deliberately dull — one `key = value` per line, `#` or `;`
 * comments, optional [sections] which are ignored, and optional quotes around
 * values so trailing spaces in a password survive:
 *
 *     # /config_keys.ini
 *     wifi_ssid     = my-network
 *     wifi_password = "hunter2 "
 *     owm_api_key   = 0123456789abcdef
 *
 * Values are never logged. The file server refuses to list or serve this file,
 * because otherwise anyone on the same network could read the keys out of it.
 */
#ifndef __APP_CONFIG_H__
#define __APP_CONFIG_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_PATH "/config_keys.ini"

/* Known keys. The names double as the .ini keys, lowercased. */
#define CFG_WIFI_SSID        "wifi_ssid"
#define CFG_WIFI_PASSWORD    "wifi_password"
#define CFG_WIFI_SSID2       "wifi_ssid2"
#define CFG_WIFI_PASSWORD2   "wifi_password2"
#define CFG_GEMINI_KEY       "gemini_api_key"
#define CFG_OWM_KEY          "owm_api_key"
#define CFG_CALENDARIFIC_KEY "calendarific_api_key"
#define CFG_CALENDAR_COUNTRY "calendar_countries"
#define CFG_ASSISTNOW_TOKEN  "ublox_assistnow_token"

/* Read the card. Safe to call before the card is mounted — it simply finds
 * nothing and leaves the compiled-in values in place. Calling again re-reads,
 * which is how a settings page could pick up an edited file. */
void cfg_load(void);

/* Value for `key`: the .ini if it has one, else the compiled-in default, else
 * an empty string. Never returns NULL, so it is safe to pass straight to
 * printf and friends. */
const char *cfg_get(const char *key);

/* True when the value is present and non-empty — the runtime equivalent of the
 * `#ifdef OWM_API_KEY` guards this replaces. */
bool cfg_has(const char *key);

/* "sd", "firmware" or "unset", for diagnostics. Never exposes the value. */
const char *cfg_source(const char *key);

#ifdef __cplusplus
}
#endif

#endif /* __APP_CONFIG_H__ */
