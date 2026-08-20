/**
 * @file      gps_assist.h
 * @brief     Assisted GNSS — tell the receiver where and when it is.
 *
 * A receiver starting with no idea of the time or its position has to search
 * blindly across every satellite and Doppler bin, which is what makes a cold
 * fix take minutes. Handing it a rough time and a rough position (UBX-MGA-INI)
 * collapses that search space, and both are things this device already knows:
 * the clock is NTP-synced at boot, and the last fix is cached in Preferences
 * for the weather app.
 *
 * Neither needs to be accurate — coarse aiding is what helps. Accuracy is
 * declared honestly in the messages so the receiver weighs them correctly.
 *
 * If a u-blox AssistNow token is configured (UBLOX_ASSISTNOW_TOKEN in
 * config_keys.h) the full ephemeris set is fetched over WiFi as well, which
 * shortens a cold start to a few seconds.
 */
#ifndef __GPS_ASSIST_H__
#define __GPS_ASSIST_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build UBX-MGA-INI-TIME_UTC into `buf` (needs >= 32 bytes). Returns the frame
 * length. `acc_s` is the claimed accuracy of the supplied time, in seconds. */
int gps_assist_build_time(uint8_t *buf, int year, int mon, int day,
                          int hour, int min, int sec, uint16_t acc_s);

/* Build UBX-MGA-INI-POS_LLH into `buf` (needs >= 28 bytes). Returns the frame
 * length. `acc_cm` is the claimed position accuracy, in centimetres. */
int gps_assist_build_pos(uint8_t *buf, double lat, double lon,
                         int32_t alt_cm, uint32_t acc_cm);

/* Send whatever aiding we can to the receiver. Call right after the module has
 * been powered up and its link re-negotiated. Brings WiFi up briefly only when
 * it would actually add something (no valid clock, or AssistNow configured). */
void gps_assist_apply(void);

#ifdef __cplusplus
}
#endif

#endif /* __GPS_ASSIST_H__ */
