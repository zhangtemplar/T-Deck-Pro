/**
 * @file      img_codec_jpg.c
 * @brief     JPEG -> 8-bit grayscale, using the TJpgDec copy vendored in LVGL.
 *
 * Same arrangement as img_codec_png.c: LVGL's lv_sjpg.c wrapper rejects
 * LV_COLOR_DEPTH 1, so LV_USE_SJPG stays off globally and we compile tjpgd.c
 * here with the guard forced on.
 *
 * TJpgDec can descale by 1/2, 1/4 or 1/8 while decoding, so an oversized photo
 * is decoded smaller instead of being refused — the panel is 240x320, so losing
 * that detail costs nothing.
 */
#undef LV_USE_SJPG
#define LV_USE_SJPG 1
#include "../../lib/lvgl/src/extra/libs/sjpg/tjpgd.c"

#include <stdlib.h>
#include <string.h>
#include "img_codec.h"

/* TJpgDec's scratch pool. The decoder needs roughly 3 KB plus the input buffer;
 * 8 KB leaves comfortable headroom for progressive-ish streams. */
#define JPG_POOL_SIZE 8192

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    uint8_t *out;       /* grayscale destination */
    int ow, oh;
} jpg_ctx_t;

static size_t jpg_in(JDEC *jd, uint8_t *buff, size_t nbyte)
{
    jpg_ctx_t *c = (jpg_ctx_t *)jd->device;
    size_t avail = c->len - c->pos;
    if (nbyte > avail) nbyte = avail;
    if (buff) memcpy(buff, c->data + c->pos, nbyte);  /* NULL means "skip" */
    c->pos += nbyte;
    return nbyte;
}

static int jpg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpg_ctx_t *c = (jpg_ctx_t *)jd->device;
    const uint8_t *src = (const uint8_t *)bitmap;   /* RGB888, JD_FORMAT 0 */

    for (int y = rect->top; y <= rect->bottom; y++) {
        if (y >= c->oh) break;
        uint8_t *dst = c->out + (size_t)y * c->ow;
        for (int x = rect->left; x <= rect->right; x++) {
            uint8_t r = src[0], g = src[1], b = src[2];
            src += 3;
            if (x < c->ow) dst[x] = IMG_LUMA(r, g, b);
        }
    }
    return 1;
}

int img_jpg_decode_gray(const uint8_t *data, size_t len,
                        uint32_t max_pixels,
                        uint8_t **out, int *w, int *h)
{
    jpg_ctx_t ctx;
    ctx.data = data;
    ctx.len  = len;
    ctx.pos  = 0;
    ctx.out  = NULL;

    void *pool = malloc(JPG_POOL_SIZE);
    if (!pool) return IMG_ERR_MEMORY;

    JDEC jd;
    JRESULT res = jd_prepare(&jd, jpg_in, pool, JPG_POOL_SIZE, &ctx);
    if (res != JDR_OK) {
        free(pool);
        return (res == JDR_MEM1 || res == JDR_MEM2) ? IMG_ERR_MEMORY : IMG_ERR_FORMAT;
    }

    /* Pick the coarsest 1/2^n that still fits the pixel budget. */
    uint8_t scale = 0;
    while (scale < 3 &&
           (uint64_t)(jd.width >> scale) * (jd.height >> scale) > max_pixels) {
        scale++;
    }
    int ow = jd.width  >> scale;
    int oh = jd.height >> scale;
    if (ow <= 0 || oh <= 0) { free(pool); return IMG_ERR_FORMAT; }
    if ((uint64_t)ow * oh > max_pixels) { free(pool); return IMG_ERR_TOO_BIG; }

    uint8_t *gray = (uint8_t *)malloc((size_t)ow * oh);
    if (!gray) { free(pool); return IMG_ERR_MEMORY; }

    ctx.out = gray;
    ctx.ow  = ow;
    ctx.oh  = oh;

    res = jd_decomp(&jd, jpg_out, scale);
    free(pool);
    if (res != JDR_OK) {
        free(gray);
        return IMG_ERR_DECODE;
    }

    *out = gray;
    *w = ow;
    *h = oh;
    return 0;
}
