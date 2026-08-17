/**
 * @file      img_render.c
 * @brief     Grayscale -> 1-bpp view rendering for the image viewer.
 *
 * Downscaling box-averages the source pixels covered by each output pixel
 * instead of point-sampling: on a black-and-white panel, aliasing that survives
 * into the dither turns fine detail into noise. Upscaling degenerates to
 * nearest-neighbour, which keeps hard pixel edges crisp when zoomed in.
 *
 * The grayscale result is then Floyd-Steinberg dithered, which is what makes
 * photographs readable at 1 bpp — a plain threshold would flatten them.
 */
#include "img_render.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

float img_view_fit_scale(int gw, int gh, int vw, int vh)
{
    if (gw <= 0 || gh <= 0) return 1.0f;
    float fw = (float)vw / (float)gw;
    float fh = (float)vh / (float)gh;
    float fit = fw < fh ? fw : fh;
    return fit < 1.0f ? fit : 1.0f;
}

void img_view_clamp(img_view_t *v, int vw, int vh)
{
    if (v->scale <= 0) return;

    float vis_w = (float)vw / v->scale;      /* source px visible across */
    float vis_h = (float)vh / v->scale;
    float max_x = (float)v->gw - vis_w;
    float max_y = (float)v->gh - vis_h;

    if (max_x <= 0)            v->off_x = 0;
    else if (v->off_x < 0)     v->off_x = 0;
    else if (v->off_x > max_x) v->off_x = max_x;

    if (max_y <= 0)            v->off_y = 0;
    else if (v->off_y < 0)     v->off_y = 0;
    else if (v->off_y > max_y) v->off_y = max_y;
}

void img_view_zoom(img_view_t *v, float factor, float min_scale, float max_scale,
                   int vw, int vh)
{
    if (v->scale <= 0) return;

    /* Anchor on the source point currently at the centre of the view. */
    float cx = v->off_x + (float)vw / (2.0f * v->scale);
    float cy = v->off_y + (float)vh / (2.0f * v->scale);

    float ns = v->scale * factor;
    if (ns < min_scale) ns = min_scale;
    if (ns > max_scale) ns = max_scale;
    if (ns == v->scale) return;
    v->scale = ns;

    v->off_x = cx - (float)vw / (2.0f * v->scale);
    v->off_y = cy - (float)vh / (2.0f * v->scale);
    img_view_clamp(v, vw, vh);
}

int img_view_render(const uint8_t *gray, const img_view_t *v,
                    uint8_t *out, int vw, int vh,
                    uint8_t ink, uint8_t bg)
{
    memset(out, bg, (size_t)vw * vh);
    if (!gray || v->gw <= 0 || v->gh <= 0 || v->scale <= 0) return 0;

    /* Extent actually covered by the image, clipped to the view. */
    int dw = (int)lroundf((float)v->gw * v->scale);
    int dh = (int)lroundf((float)v->gh * v->scale);
    if (dw > vw) dw = vw;
    if (dh > vh) dh = vh;
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int ox = (vw - dw) / 2;
    int oy = (vh - dh) / 2;

    /* Floyd-Steinberg carries error into the next row, so keep two rows of
     * accumulated error. Units are 1/16 of a grey level. */
    int *err_cur  = (int *)calloc((size_t)dw + 2, sizeof(int));
    int *err_next = (int *)calloc((size_t)dw + 2, sizeof(int));
    if (!err_cur || !err_next) { free(err_cur); free(err_next); return -1; }

    const float inv = 1.0f / v->scale;      /* source px per output px */

    for (int dy = 0; dy < dh; dy++) {
        float sy0f = v->off_y + (float)dy * inv;
        int sy0 = (int)sy0f;
        int sy1 = (int)ceilf(sy0f + inv);
        if (sy0 < 0) sy0 = 0;
        if (sy1 > v->gh) sy1 = v->gh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy0 >= v->gh) break;

        for (int dx = 0; dx < dw; dx++) {
            float sx0f = v->off_x + (float)dx * inv;
            int sx0 = (int)sx0f;
            int sx1 = (int)ceilf(sx0f + inv);
            if (sx0 < 0) sx0 = 0;
            if (sx1 > v->gw) sx1 = v->gw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx0 >= v->gw) continue;

            uint32_t sum = 0, cnt = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint8_t *srow = gray + (size_t)sy * v->gw;
                for (int sx = sx0; sx < sx1; sx++) { sum += srow[sx]; cnt++; }
            }
            int val = cnt ? (int)(sum / cnt) : 255;

            int wanted = val + err_cur[dx + 1] / 16;
            int outv = wanted < 128 ? 0 : 255;
            int e = wanted - outv;

            err_cur[dx + 2]  += e * 7;
            err_next[dx]     += e * 3;
            err_next[dx + 1] += e * 5;
            err_next[dx + 2] += e * 1;

            if (outv == 0) out[(size_t)(oy + dy) * vw + (ox + dx)] = ink;
        }

        int *t = err_cur; err_cur = err_next; err_next = t;
        memset(err_next, 0, ((size_t)dw + 2) * sizeof(int));
    }

    free(err_cur);
    free(err_next);
    return 0;
}
