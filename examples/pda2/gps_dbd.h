/**
 * @file      gps_dbd.h
 * @brief     Carry the receiver's navigation database across a power cut.
 *
 * BOARD_GPS_EN gates the whole GPS module, backup supply included, so every
 * time the rail drops the receiver loses its battery-backed RAM: ephemeris,
 * almanac, time and last position. It then has to search blind, which is the
 * "acquiring from scratch every time" that made the app feel broken.
 *
 * UBX-MGA-DBD is u-blox's answer for exactly this arrangement. The host polls
 * the navigation database out of the receiver, keeps the opaque bytes, and
 * feeds them back after the next power-up. The receiver validates what it
 * gets, so stale data is discarded rather than believed. No network, no
 * AssistNow token, nothing to subscribe to.
 *
 * The dump is stored in NVS rather than on the SD card: it has to survive the
 * battery going flat, and the card may not be mounted when the rail drops.
 *
 * These must only be called with the GPS parser task suspended — they read
 * the UART directly and would otherwise race it.
 */
#ifndef __GPS_DBD_H__
#define __GPS_DBD_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Poll the navigation database and store it. Call before cutting the rail.
 * Returns true if something was saved. */
bool gps_dbd_save(void);

/* Feed a stored database back. Call after the link is up and after the
 * MGA-INI time aiding, which u-blox wants sent first. Returns true if a
 * usable dump was found and sent. */
bool gps_dbd_restore(void);

/* Age of the stored dump in seconds, or -1 if there isn't one. */
long gps_dbd_age_s(void);

#ifdef __cplusplus
}
#endif

#endif /* __GPS_DBD_H__ */
