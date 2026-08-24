/**
 * @file      garmin_img.h
 * @brief     Reader for classic (unencrypted) Garmin .img vector maps.
 *
 * Reads the format mkgmap produces, which is what you get from
 * garmin.opentopomap.org and every other OSM-derived Garmin map. Garmin's own
 * newer "NT" maps are a different, undocumented encoding and are rejected at
 * open rather than mis-drawn.
 *
 * Structure of an .img, and therefore of this reader:
 *
 *   - A pseudo-FAT at 0x400: one 512-byte directory entry per subfile, giving
 *     an 8.3 name, a length, and the list of blocks holding it. Subfiles are
 *     scattered across the file in block-sized pieces, so every read has to go
 *     through that list.
 *   - Per map tile, a TRE/RGN/LBL triple. TRE holds the bounding box, the zoom
 *     levels, and the subdivisions: rectangles with a centre, a half-size, and
 *     an offset into RGN. RGN holds the geometry.
 *   - Geometry is a bitstream of deltas relative to the subdivision centre,
 *     with a per-object bit width. Decoding it is the fiddly part; see
 *     read_coord_offset() in the .cpp.
 *
 * What makes this practical on a device with no room to spare: the subdivision
 * table is a flat list of boxes, so a viewport query is a linear scan over a
 * few thousand 16-byte records, and only the subdivisions that overlap get
 * their geometry read off the SD card. Nothing else is resident.
 *
 * Coordinates come out as degrees x 1e7, matching the GPS code, rather than as
 * the 2^24-per-360-degrees units the file uses.
 */
#ifndef __GARMIN_IMG_H__
#define __GARMIN_IMG_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Object classes, as they appear in RGN. */
enum {
    GIMG_POINT    = 0,
    GIMG_IPOINT   = 1,   /* indexed point */
    GIMG_POLYLINE = 2,
    GIMG_POLYGON  = 3,
};

/* Longest run handed to the callback at once. Garmin polylines can be longer;
 * they arrive split into consecutive calls with one point of overlap so the
 * line still joins up. */
#define GIMG_MAX_POINTS 300

/**
 * @param type   Garmin type code (0x20/0x21/0x22 are contours, 0x01-0x0d
 *               roads, 0x18-0x1f water, and so on)
 * @param kind   GIMG_POLYLINE or GIMG_POLYGON
 * @param lat,lon  degrees x 1e7
 */
typedef void (*gimg_feature_cb)(uint8_t type, uint8_t kind,
                                const int32_t *lat, const int32_t *lon,
                                int n_points, void *user);

/* Open a .img. Only one at a time; opening closes any previous. */
bool gimg_open(const char *path);
void gimg_close(void);
bool gimg_is_open(void);

/* Bounding box across every tile, degrees x 1e7. */
void gimg_bounds(int32_t *min_lat, int32_t *min_lon,
                 int32_t *max_lat, int32_t *max_lon);

/* Distinct zoom levels, coarsest (0) to finest. Garmin calls these "map
 * levels"; each has its own set of subdivisions and its own coordinate
 * resolution. */
int gimg_level_count(void);

/* How many tiles a viewport overlaps. Pure in-RAM test over the tile index, no
 * I/O — the UI uses it to refuse a zoom-out that would cost more seeks than a
 * redraw can afford. */
int gimg_overlap_tiles(int32_t min_lat, int32_t min_lon, int32_t max_lat, int32_t max_lon);

/* Level best suited to drawing this viewport: the finest one whose overlapping
 * subdivisions stay within a redraw budget. Takes the box rather than a span
 * because which subdivisions are touched, not how wide the view is, decides
 * how much geometry has to be decoded. */
int gimg_pick_level(int32_t min_lat, int32_t min_lon, int32_t max_lat, int32_t max_lon);

/* Visit every polyline and polygon at `level` whose subdivision overlaps the
 * box. Returns features emitted, -1 if no map is open, and 0 if the level
 * holds nothing here. */
int gimg_query(int level,
               int32_t min_lat, int32_t min_lon,
               int32_t max_lat, int32_t max_lon,
               gimg_feature_cb cb, void *user);

#ifdef GIMG_STATS
/* Work counters, for sizing the redraw budget. Zero them yourself. */
typedef struct {
    unsigned long reads;    /* calls into the file layer (SD seeks)   */
    unsigned long bytes;    /* bytes pulled off the card              */
    unsigned long bits;     /* bits pulled through the bit reader     */
    unsigned long points;   /* vertices reconstructed                 */
    unsigned long subdivs;  /* subdivisions visited                   */
} gimg_stats_t;
extern gimg_stats_t g_gimg_stats;
#endif

/* One-line summary for the UI: tile count, subdivisions, levels. */
const char *gimg_describe(void);

#ifdef __cplusplus
}
#endif

#endif /* __GARMIN_IMG_H__ */
