/**
 * @file      map_draw.h
 * @brief     Draw Garmin map geometry into a 1-byte-per-pixel canvas.
 *
 * Deliberately not built on lv_canvas_draw_line. A viewport routinely holds
 * several thousand short segments, and each LVGL line call sets up a draw
 * descriptor, a mask and a blend — tens of microseconds apiece, which would
 * put the raster pass well above 100 ms and make it, not the panel, the reason
 * a pan felt slow. Clipping plus Bresenham straight into the buffer is a few
 * cycles per pixel and takes the same work down to single-digit milliseconds.
 *
 * The projection is equirectangular about the view centre with longitude
 * scaled by cos(latitude), so north is up and one pixel is the same distance
 * on both axes — the same correction the track plot needs.
 *
 * Takes a plain byte buffer rather than an lv_obj_t so the geometry can be
 * exercised on the host. That is sound because LV_COLOR_DEPTH is 1 here, which
 * makes lv_color_t exactly one byte; map_draw_begin checks it.
 */
#ifndef __MAP_DRAW_H__
#define __MAP_DRAW_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set up a view. `lat_span` is the latitude range the canvas height covers,
 * in degrees x 1e7; longitude follows from the aspect ratio so the scale is
 * uniform. `black`/`white` are the byte values to write. */
void map_draw_begin(uint8_t *buf, int w, int h,
                    int32_t centre_lat, int32_t centre_lon, int32_t lat_span,
                    uint8_t black, uint8_t white);

/* Clear to the background colour. */
void map_draw_clear(void);

/* Draw one feature. Coordinates are degrees x 1e7. `type` is the Garmin type
 * code and picks the line weight and dash. */
void map_draw_feature(uint8_t type, uint8_t kind,
                      const int32_t *lat, const int32_t *lon, int n_points);

/* Contour lines on or off. They dominate the geometry and, with no elevation
 * labels, carry shape without magnitude — worth being able to drop. */
void map_draw_set_contours(bool on);
bool map_draw_contours(void);

/* Draw a polyline in a fixed style, for the recorded track. */
void map_draw_track(const int32_t *lat, const int32_t *lon, int n_points,
                    int thickness);

/* North arrow, top-right. */
void map_draw_north(void);

/* Open crosshair at the view centre, where the status coordinates refer to. */
void map_draw_centre_mark(int size);

/* A crosshair at a position, for the current fix. */
void map_draw_marker(int32_t lat, int32_t lon, int size);

/* A scale bar in the bottom-left, snapping to a round distance. Writes the
 * label it chose into `label` (e.g. "500m", "2km"). */
void map_draw_scale_bar(char *label, int cap);

/* Segments actually rasterised since the last begin(), for tuning. */
unsigned long map_draw_segments(void);

/* Project a coordinate to canvas pixels. Returns false if far off-canvas. */
bool map_draw_project(int32_t lat, int32_t lon, int *x, int *y);

#ifdef __cplusplus
}
#endif

#endif /* __MAP_DRAW_H__ */
