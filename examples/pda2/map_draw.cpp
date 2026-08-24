/**
 * @file      map_draw.cpp
 * @brief     Bresenham rasteriser for Garmin geometry. See map_draw.h.
 */
#include "map_draw.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static uint8_t *s_buf;
static int      s_w, s_h;
static uint8_t  s_black = 0, s_white = 1;
static int32_t  s_clat, s_clon;
static float    s_sy;          /* pixels per 1e-7 degree of latitude  */
static float    s_sx;          /* ditto longitude, already cos-scaled */
static unsigned long s_segs;

/* Metres per degree of latitude, for the scale bar. */
#define M_PER_DEG_LAT 110574.0f

void map_draw_begin(uint8_t *buf, int w, int h,
                    int32_t centre_lat, int32_t centre_lon, int32_t lat_span,
                    uint8_t black, uint8_t white)
{
    s_buf = buf; s_w = w; s_h = h;
    s_black = black; s_white = white;
    s_clat = centre_lat; s_clon = centre_lon;
    s_segs = 0;

    if (lat_span < 100) lat_span = 100;            /* ~1 m; keeps the scale finite */
    s_sy = (float)h / (float)lat_span;

    float coslat = cosf((float)centre_lat / 1e7f * 0.017453293f);
    if (coslat < 0.02f) coslat = 0.02f;            /* keep the poles finite */
    s_sx = s_sy * coslat;
}

void map_draw_clear(void)
{
    if (s_buf) memset(s_buf, s_white, (size_t)s_w * s_h);
}

unsigned long map_draw_segments(void) { return s_segs; }

bool map_draw_project(int32_t lat, int32_t lon, int *x, int *y)
{
    /* Longitude difference in int64 so a view straddling the antimeridian —
     * which is exactly where Fiji is — does not overflow before wrapping. */
    int64_t dlon = (int64_t)lon - s_clon;
    if (dlon >  1800000000LL) dlon -= 3600000000LL;
    if (dlon < -1800000000LL) dlon += 3600000000LL;

    float fx = s_w * 0.5f + (float)dlon * s_sx;
    float fy = s_h * 0.5f - (float)((int64_t)lat - s_clat) * s_sy;

    /* Reject the wildly out-of-range before the cast, where the conversion
     * would be undefined rather than merely useless. */
    if (fx < -32000.0f || fx > 32000.0f || fy < -32000.0f || fy > 32000.0f) return false;
    *x = (int)lrintf(fx);
    *y = (int)lrintf(fy);
    return true;
}

static inline void px(int x, int y)
{
    if ((unsigned)x < (unsigned)s_w && (unsigned)y < (unsigned)s_h)
        s_buf[y * s_w + x] = s_black;
}

/* Cohen-Sutherland, so a segment mostly off-canvas costs a few comparisons
 * instead of a long Bresenham walk that writes nothing. */
static int outcode(int x, int y)
{
    int c = 0;
    if (x < 0) c |= 1; else if (x >= s_w) c |= 2;
    if (y < 0) c |= 4; else if (y >= s_h) c |= 8;
    return c;
}

static bool clip(int *x0, int *y0, int *x1, int *y1)
{
    int c0 = outcode(*x0, *y0), c1 = outcode(*x1, *y1);
    for (int guard = 0; guard < 8; guard++) {
        if (!(c0 | c1)) return true;             /* both inside  */
        if (c0 & c1)    return false;            /* both outside, same side */

        int c = c0 ? c0 : c1;
        int x = 0, y = 0;
        if (c & 8)      { x = *x0 + (*x1 - *x0) * (s_h - 1 - *y0) / (*y1 - *y0); y = s_h - 1; }
        else if (c & 4) { x = *x0 + (*x1 - *x0) * (0 - *y0) / (*y1 - *y0);       y = 0; }
        else if (c & 2) { y = *y0 + (*y1 - *y0) * (s_w - 1 - *x0) / (*x1 - *x0); x = s_w - 1; }
        else            { y = *y0 + (*y1 - *y0) * (0 - *x0) / (*x1 - *x0);       x = 0; }

        if (c == c0) { *x0 = x; *y0 = y; c0 = outcode(x, y); }
        else         { *x1 = x; *y1 = y; c1 = outcode(x, y); }
    }
    return false;
}

/* Pen: `on` pixels drawn then `off` skipped, repeating. `off` of 0 is solid.
 *
 * The phase is a file-static rather than a local because a polyline arrives as
 * a run of short segments: restarting the pattern at every vertex would draw
 * the "on" part each time and a dotted line would come out solid. Callers reset
 * it once per feature. */
static int s_dash_phase;

static void line(int x0, int y0, int x1, int y1, int thick, int on, int off)
{
    if (!clip(&x0, &y0, &x1, &y1)) return;
    s_segs++;

    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;

    const int period = on + off;
    int err = dx - dy;
    for (;;) {
        if (!off || (s_dash_phase % period) < on) {
            px(x0, y0);
            if (thick > 1) { px(x0 + 1, y0); px(x0, y0 + 1); }
        }
        s_dash_phase++;
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Contours are the one class worth turning off wholesale: they outnumber
 * everything else several to one and, without elevation labels, they carry
 * shape but no magnitude. Off unless asked for; the 'c' key toggles it and the
 * choice persists. */
static bool s_contours = false;

void map_draw_set_contours(bool on) { s_contours = on; }
bool map_draw_contours(void)        { return s_contours; }

/* Powerlines cross terrain without saying anything about it. */
#define SKIP_POWERLINES 1

static bool is_contour(uint8_t type)
{
    return type >= 0x20 && type <= 0x25;    /* land and depth, all three grades */
}

static bool skip_feature(uint8_t type, uint8_t kind)
{
    if (kind != 3 && is_contour(type) && !s_contours) return true;
#if SKIP_POWERLINES
    if (kind != 3 && type == 0x29) return true;
#endif
    return false;
}

static void pen_for(uint8_t type, uint8_t kind, int *thick, int *on, int *off)
{
    /* Three pens, so the classes stay apart at a glance on a two-colour panel:
     *   long dash  natural and administrative edges — coastline, rivers,
     *              boundaries, area outlines
     *   dotted     ways you walk, and contours
     *   solid      things you drive or ride on
     * Motorways and runways get a second pixel of weight, being the features
     * you want to find without looking for them. */
    const int LONG_ON = 5, LONG_OFF = 3;
    const int DOT_ON  = 1, DOT_OFF  = 2;

    *thick = 1;
    *on = 1; *off = 0;                       /* solid 1 px by default */

    if (kind == 3) { *on = LONG_ON; *off = LONG_OFF; return; }   /* polygon outline */

    switch (type) {
    case 0x01:
    case 0x02: *thick = 2; break;                     /* major / principal highway */
    case 0x27: *thick = 2; break;                     /* airport runway            */

    case 0x0a:                                        /* unpaved road              */
    case 0x16:                                        /* trail                     */
    case 0x26: *on = DOT_ON; *off = DOT_OFF; break;   /* intermittent stream       */

    case 0x15:                                        /* shoreline                 */
    case 0x18:                                        /* stream                    */
    case 0x1f:                                        /* river                     */
    case 0x1c:
    case 0x1d:
    case 0x1e:                                        /* boundaries                */
    case 0x28: *on = LONG_ON; *off = LONG_OFF; break; /* pipeline                  */

    default:
        if (is_contour(type)) { *on = DOT_ON; *off = DOT_OFF; }
        break;                                        /* roads and railways: solid */
    }
}

void map_draw_feature(uint8_t type, uint8_t kind,
                      const int32_t *lat, const int32_t *lon, int n_points)
{
    if (!s_buf || n_points < 2) return;
    if (skip_feature(type, kind)) return;

    int thick, on, off;
    pen_for(type, kind, &thick, &on, &off);
    s_dash_phase = 0;                 /* one pattern per feature, not per segment */

    int px0, py0;
    if (!map_draw_project(lat[0], lon[0], &px0, &py0)) return;

    for (int i = 1; i < n_points; i++) {
        int x, y;
        if (!map_draw_project(lat[i], lon[i], &x, &y)) {
            /* Skip the vertex but keep the pen where it was, so one bad point
             * breaks the line rather than dragging it across the screen. */
            continue;
        }
        /* Drop vertices that land on the pixel we are already at. At anything
         * but full zoom this removes most of the geometry for free. */
        if (x != px0 || y != py0) {
            line(px0, py0, x, y, thick, on, off);
            px0 = x; py0 = y;
        }
    }
}

void map_draw_track(const int32_t *lat, const int32_t *lon, int n_points, int thickness)
{
    if (!s_buf || n_points < 2) return;
    s_dash_phase = 0;
    int px0, py0;
    if (!map_draw_project(lat[0], lon[0], &px0, &py0)) return;
    for (int i = 1; i < n_points; i++) {
        int x, y;
        if (!map_draw_project(lat[i], lon[i], &x, &y)) continue;
        line(px0, py0, x, y, thickness, 1, 0);
        px0 = x; py0 = y;
    }
}

void map_draw_marker(int32_t lat, int32_t lon, int size)
{
    int x, y;
    if (!map_draw_project(lat, lon, &x, &y)) return;
    for (int d = -size; d <= size; d++) {
        px(x + d, y);
        px(x, y + d);
    }
    /* A ring, so the mark stays visible over dense contour hatching. */
    for (int a = 0; a < 360; a += 20)
        px(x + (int)lrintf((size + 2) * cosf(a * 0.017453293f)),
           y + (int)lrintf((size + 2) * sinf(a * 0.017453293f)));
}

void map_draw_north(void)
{
    /* Top-right. The plot is north-up by construction, but on a screen of
     * unlabelled contours there is nothing to infer that from. */
    /* Arrow only. The "N" is drawn by the caller through LVGL: plotting a
     * letter pixel by pixel at this size came out unreadable. */
    const int x = s_w - 10, y0 = 6, y1 = 24;
    for (int y = y0; y <= y1; y++) px(x, y);
    for (int i = 0; i <= 3; i++) { px(x - i, y0 + i); px(x + i, y0 + i); }
}

void map_draw_centre_mark(int size)
{
    /* Marks where the coordinates in the status line refer to, and gives the
     * eye something to hold on to while panning. Hollow, so it cannot be
     * confused with the filled GPS marker. */
    int x = s_w / 2, y = s_h / 2;
    for (int d = size; d <= size + 4; d++) {
        px(x + d, y); px(x - d, y);
        px(x, y + d); px(x, y - d);
    }
}

void map_draw_scale_bar(char *label, int cap)
{
    static const float nice[] = {10, 20, 50, 100, 200, 500, 1000, 2000,
                                 5000, 10000, 20000, 50000, 100000};
    const int max_px = 70;

    /* Pixels per metre, from the latitude scale: it is the axis that is not
     * cosine-corrected, so it converts directly. */
    float px_per_m = s_sy * 1e7f / M_PER_DEG_LAT;

    float metres = nice[0];
    for (unsigned i = 0; i < sizeof(nice) / sizeof(nice[0]); i++) {
        if (nice[i] * px_per_m <= max_px) metres = nice[i];
        else break;
    }
    int len = (int)lrintf(metres * px_per_m);
    if (len < 4) { if (label && cap) label[0] = 0; return; }

    int y = s_h - 6, x0 = 5;
    for (int x = x0; x <= x0 + len; x++) px(x, y);
    for (int t = 1; t <= 3; t++) { px(x0, y - t); px(x0 + len, y - t); }

    if (label && cap) {
        if (metres >= 1000) snprintf(label, cap, "%gkm", metres / 1000);
        else                snprintf(label, cap, "%gm", metres);
    }
}
