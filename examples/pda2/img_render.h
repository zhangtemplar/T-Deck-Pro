/**
 * @file      img_render.h
 * @brief     Resample + dither an 8-bit grayscale image into a 1-bpp view.
 *
 * Split out from the viewer UI so the zoom/pan arithmetic is plain C with no
 * LVGL dependency (and can be exercised off-device).
 */
#ifndef __IMG_RENDER_H__
#define __IMG_RENDER_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int   gw, gh;        /* source image size, px                       */
    float scale;         /* display px per source px (1.0 = 1:1)        */
    float off_x, off_y;  /* source coord shown at the view's top-left   */
} img_view_t;

/* The scale at which the whole image just fits in vw x vh, capped at 1.0 so a
 * small image is shown at its native size rather than blown up. */
float img_view_fit_scale(int gw, int gh, int vw, int vh);

/* Keep the visible window over the image: pins an axis to 0 when the image is
 * smaller than the view on that axis, otherwise clamps to the far edge. */
void img_view_clamp(img_view_t *v, int vw, int vh);

/* Zoom by `factor` about the centre of the view, clamped to [min_scale, max_scale]. */
void img_view_zoom(img_view_t *v, float factor, float min_scale, float max_scale,
                   int vw, int vh);

/* Box-filter resample the visible region and Floyd-Steinberg dither it into
 * `out` (vw*vh bytes, row-major). Pixels are written as `ink` or `bg`; the whole
 * buffer is cleared to `bg` first, and the image is centred when it does not
 * fill the view. Returns 0 on success, -1 if scratch allocation failed. */
int img_view_render(const uint8_t *gray, const img_view_t *v,
                    uint8_t *out, int vw, int vh,
                    uint8_t ink, uint8_t bg);

#ifdef __cplusplus
}
#endif

#endif /* __IMG_RENDER_H__ */
