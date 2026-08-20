/**
 * @file      world_map.h
 * @brief     Coarse world coastline, shared by the GPS and world-clock screens.
 *
 * Major continent outlines as lat/lon polyline segments {lat1, lon1, lat2, lon2}
 * in whole degrees — enough to recognise the continents at 240 px wide, and
 * small enough to keep in flash. Kept in one place so both screens draw the
 * same map.
 */
#ifndef __WORLD_MAP_H__
#define __WORLD_MAP_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const int16_t world_coastline[][4];
extern const int      world_coastline_count;

#ifdef __cplusplus
}
#endif

#endif /* __WORLD_MAP_H__ */
