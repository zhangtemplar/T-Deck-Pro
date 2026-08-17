/**
 * @file      img_codec_bmp.c
 * @brief     Uncompressed BMP -> 8-bit grayscale.
 *
 * Handles the variants that actually turn up on this device: 1/4/8-bit palette
 * (which is what our own screenshots are — see screenshot.cpp), plus 24- and
 * 32-bit true colour, in either row order. Compressed BMPs (RLE, BI_BITFIELDS
 * with non-standard masks) are rejected rather than mis-rendered.
 */
#include <stdlib.h>
#include <string.h>
#include "img_codec.h"

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int img_bmp_decode_gray(const uint8_t *data, size_t len,
                        uint32_t max_pixels,
                        uint8_t **out, int *w, int *h)
{
    if (len < 54 || data[0] != 'B' || data[1] != 'M') return IMG_ERR_FORMAT;

    uint32_t data_off = rd32(data + 10);
    uint32_t hdr_size = rd32(data + 14);
    if (hdr_size < 12) return IMG_ERR_FORMAT;

    int32_t  bw, bh;
    uint16_t bpp;
    uint32_t compression = 0;
    uint32_t palette_n = 0;

    if (hdr_size == 12) {                  /* BITMAPCOREHEADER */
        bw  = (int16_t)rd16(data + 18);
        bh  = (int16_t)rd16(data + 20);
        bpp = rd16(data + 24);
    } else {                               /* BITMAPINFOHEADER and later */
        bw  = (int32_t)rd32(data + 18);
        bh  = (int32_t)rd32(data + 22);
        bpp = rd16(data + 28);
        compression = rd32(data + 30);
        palette_n   = rd32(data + 46);
    }

    /* BI_RGB only. BI_BITFIELDS (3) at 32bpp is usually plain BGRX, but we
     * can't rely on that without parsing the masks, so refuse it. */
    if (compression != 0) return IMG_ERR_UNSUPP;

    int top_down = 0;
    if (bh < 0) { top_down = 1; bh = -bh; }
    if (bw <= 0 || bh <= 0) return IMG_ERR_FORMAT;
    if ((uint64_t)bw * bh > max_pixels) return IMG_ERR_TOO_BIG;

    const uint8_t *pal = NULL;
    uint32_t pal_entry = (hdr_size == 12) ? 3 : 4;   /* RGB vs RGBQUAD */
    if (bpp <= 8) {
        if (palette_n == 0) palette_n = 1u << bpp;
        pal = data + 14 + hdr_size;
        if ((size_t)(pal - data) + (size_t)palette_n * pal_entry > len) return IMG_ERR_FORMAT;
    } else if (bpp != 24 && bpp != 32) {
        return IMG_ERR_UNSUPP;
    }

    size_t stride = (((size_t)bw * bpp + 31) / 32) * 4;   /* rows are 4-byte aligned */
    if (data_off > len || stride * (size_t)bh > len - data_off) return IMG_ERR_FORMAT;
    const uint8_t *px = data + data_off;

    uint8_t *gray = (uint8_t *)malloc((size_t)bw * bh);
    if (!gray) return IMG_ERR_MEMORY;

    /* Pre-flatten the palette to luma so the inner loop is a single lookup. */
    uint8_t lut[256];
    if (pal) {
        for (uint32_t i = 0; i < 256; i++) {
            if (i < palette_n) {
                const uint8_t *e = pal + i * pal_entry;   /* stored B,G,R */
                lut[i] = IMG_LUMA(e[2], e[1], e[0]);
            } else {
                lut[i] = 0;
            }
        }
    }

    for (int y = 0; y < bh; y++) {
        /* BMP rows run bottom-up unless the height was negative. */
        const uint8_t *row = px + (size_t)(top_down ? y : (bh - 1 - y)) * stride;
        uint8_t *dst = gray + (size_t)y * bw;

        switch (bpp) {
        case 1:
            for (int x = 0; x < bw; x++)
                dst[x] = lut[(row[x >> 3] >> (7 - (x & 7))) & 1];
            break;
        case 4:
            for (int x = 0; x < bw; x++)
                dst[x] = lut[(x & 1) ? (row[x >> 1] & 0x0F) : (row[x >> 1] >> 4)];
            break;
        case 8:
            for (int x = 0; x < bw; x++)
                dst[x] = lut[row[x]];
            break;
        case 24:
            for (int x = 0; x < bw; x++)
                dst[x] = IMG_LUMA(row[x * 3 + 2], row[x * 3 + 1], row[x * 3]);
            break;
        default: /* 32 */
            for (int x = 0; x < bw; x++)
                dst[x] = IMG_LUMA(row[x * 4 + 2], row[x * 4 + 1], row[x * 4]);
            break;
        }
    }

    *out = gray;
    *w = bw;
    *h = bh;
    return 0;
}
