/**
 * @file      ui_gps_enhanced.h
 * @brief     State the GPS app exposes to the rest of the firmware.
 */
#ifndef __UI_GPS_ENHANCED_H__
#define __UI_GPS_ENHANCED_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True while a track is being recorded. The idle manager treats this as work
 * in progress: a recording needs the GPS rail powered and the CPU at speed,
 * and it happens precisely when nobody is touching the device. */
bool gps_track_is_recording(void);

#ifdef __cplusplus
}
#endif

#endif /* __UI_GPS_ENHANCED_H__ */
