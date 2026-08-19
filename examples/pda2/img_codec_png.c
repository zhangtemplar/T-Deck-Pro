/**
 * @file      img_codec_png.c
 * @brief     PNG -> 8-bit grayscale, using the lodepng copy vendored in LVGL.
 *
 * LVGL ships lodepng under extra/libs/png but compiles it only when LV_USE_PNG
 * is set, and that also drags in lv_png.c, which does not build at
 * LV_COLOR_DEPTH 1. So we keep LV_USE_PNG off globally and pull lodepng.c into
 * this translation unit with the guard forced on, which gives us the codec
 * without LVGL's unusable decoder shim.
 *
 * Note we decode to RGB and compute luma ourselves: asking lodepng for LCT_GREY
 * does NOT convert colour to grayscale, it just keeps the red channel
 * (see rgba8ToPixel / convertRGBA in lodepng.c).
 */

/* Trim the parts of lodepng we don't use — encoder, file I/O, C++ wrapper and
 * error strings — before lodepng.h picks its defaults. */
#define LODEPNG_NO_COMPILE_ENCODER
#define LODEPNG_NO_COMPILE_DISK
#define LODEPNG_NO_COMPILE_CPP
#define LODEPNG_NO_COMPILE_ERROR_TEXT

/* Supply our own allocators. LVGL's copy of lodepng routes them to
 * lv_mem_alloc, i.e. LVGL's fixed 512 KB UI pool (LV_MEM_CUSTOM 0) — far too
 * small for a decoded image, and it would fight the widgets for space. Worse,
 * this file frees/reallocs the result with the libc functions, which must not
 * be mixed with lv_mem pointers. Routing everything through malloc keeps the
 * pair consistent and reaches PSRAM: the ESP32 build has
 * CONFIG_SPIRAM_USE_MALLOC=y with ALWAYSINTERNAL=4096, so the big buffers land
 * in PSRAM automatically while small ones stay in fast internal RAM. */
#define LODEPNG_NO_COMPILE_ALLOCATORS

/* Force the vendored source on regardless of the project's LVGL config. */
#undef LV_USE_PNG
#define LV_USE_PNG 1
#include "../../lib/lvgl/src/extra/libs/png/lodepng.c"

#include <stdlib.h>
#include "img_codec.h"
#include "inflate_util.h"

/* The allocators lodepng.c declared but, per LODEPNG_NO_COMPILE_ALLOCATORS,
 * left for us to define. */
void *lodepng_malloc(size_t size)             { return malloc(size); }
void *lodepng_realloc(void *ptr, size_t size) { return realloc(ptr, size); }
void  lodepng_free(void *ptr)                 { free(ptr); }

/* Raw DEFLATE, exposed for the EPUB reader (see inflate_util.h). This lives
 * here because this file is the only place lodepng is compiled. */
int inflate_raw(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len)
{
    unsigned char *buf = NULL;
    size_t n = 0;
    unsigned err = lodepng_inflate(&buf, &n, in, in_len,
                                   &lodepng_default_decompress_settings);
    if (err) {
        free(buf);
        return -1;
    }
    *out = buf;
    *out_len = n;
    return 0;
}

int img_png_decode_gray(const uint8_t *data, size_t len,
                        uint32_t max_pixels,
                        uint8_t **out, int *w, int *h)
{
    unsigned iw = 0, ih = 0;

    /* Read just the IHDR first so an oversized image is rejected before we
     * commit to allocating anything. */
    LodePNGState st;
    lodepng_state_init(&st);
    unsigned err = lodepng_inspect(&iw, &ih, &st, data, len);
    lodepng_state_cleanup(&st);
    if (err) return IMG_ERR_FORMAT;
    if (iw == 0 || ih == 0) return IMG_ERR_FORMAT;
    if ((uint64_t)iw * ih > max_pixels) return IMG_ERR_TOO_BIG;

    unsigned char *rgb = NULL;
    err = lodepng_decode_memory(&rgb, &iw, &ih, data, len, LCT_RGB, 8);
    if (err) {
        if (rgb) free(rgb);
        return (err == 83) ? IMG_ERR_MEMORY : IMG_ERR_DECODE;
    }

    /* RGB -> luma in place. The write cursor always trails the read cursor
     * (1 byte consumed per 3 produced), so this is safe without a second
     * buffer, and lets us shrink the allocation afterwards. */
    size_t n = (size_t)iw * ih;
    for (size_t i = 0; i < n; i++) {
        rgb[i] = IMG_LUMA(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
    }

    unsigned char *gray = (unsigned char *)realloc(rgb, n);
    *out = gray ? gray : rgb;   /* shrink is best-effort; original stays valid */
    *w = (int)iw;
    *h = (int)ih;
    return 0;
}
