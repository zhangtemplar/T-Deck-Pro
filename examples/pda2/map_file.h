/**
 * @file      map_file.h
 * @brief     Reader for .tdmap vector maps on the SD card.
 *
 * The format is described in full at the top of tools/gen_map.py, which is
 * what writes these files. In short: a bounding box, a handful of zoom levels,
 * and for each level a flat grid of tiles indexed by (row, col). A tile holds
 * polylines delta-encoded as zigzag varints in tile-local units.
 *
 * Nothing is held in memory but the header, the level table and one tile's
 * worth of decoded points, so map size is bounded by the SD card rather than
 * by RAM. A query reads only the tiles the viewport actually touches.
 */
#ifndef __MAP_FILE_H__
#define __MAP_FILE_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Feature classes, matching TYPE_* in tools/gen_map.py. */
enum {
    MAP_COASTLINE     = 0,
    MAP_WATER         = 1,
    MAP_WATERWAY      = 2,
    MAP_ROAD_MAJOR    = 3,
    MAP_ROAD_MINOR    = 4,
    MAP_PATH          = 5,
    MAP_RAIL          = 6,
    MAP_BUILDING      = 7,
    MAP_LANDUSE       = 8,
    MAP_CONTOUR       = 9,
    MAP_CONTOUR_INDEX = 10,
    MAP_PLACE         = 11,
    MAP_BOUNDARY      = 12,
    MAP_TYPE_COUNT    = 13,
};

#define MAP_FLAG_CLOSED 0x01

/* Longest run of points handed to the callback in one go. The writer splits
 * anything longer, so this is a hard bound, not a truncation. */
#define MAP_MAX_POINTS 256

/* Called once per feature. Coordinates are absolute, degrees x 1e7. */
typedef void (*map_feature_cb)(uint8_t type, uint8_t flags,
                               const int32_t *lat, const int32_t *lon,
                               int n_points, void *user);

/* Open a .tdmap. Only one may be open at a time; opening closes any previous.
 * Returns false and leaves nothing open if the file is missing or malformed. */
bool map_open(const char *path);
void map_close(void);
bool map_is_open(void);

/* Bounding box of the open map, degrees x 1e7. */
void map_bounds(int32_t *min_lat, int32_t *min_lon,
                int32_t *max_lat, int32_t *max_lon);

int  map_level_count(void);

/* Tile span of a level, degrees x 1e7, or 0 if the index is out of range.
 * Level 0 is the coarsest. */
int32_t map_level_span(int level);

/* The level whose detail suits showing `lat_span` (degrees x 1e7) across
 * `px` pixels — the finest one whose simplification is still below what the
 * screen can resolve, so we neither draw invisible detail nor a blocky
 * outline. Returns a valid level whenever a map is open. */
int map_pick_level(int32_t lat_span, int px);

/* Visit every feature at `level` whose tile overlaps the box. Features are
 * clipped to tile edges by the writer, so a long road arrives as one run per
 * tile it crosses. Returns the number of features visited, or -1 on error. */
int map_query(int level,
              int32_t min_lat, int32_t min_lon,
              int32_t max_lat, int32_t max_lon,
              map_feature_cb cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* __MAP_FILE_H__ */
