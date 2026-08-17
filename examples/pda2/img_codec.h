/**
 * @file      img_codec.h
 * @brief     PNG / JPEG / BMP -> 8-bit grayscale decoders for the image viewer.
 *
 * Every decoder takes the whole encoded file in memory and produces a freshly
 * allocated 8-bit grayscale bitmap (one byte per pixel, row-major, top-down).
 * Colour input is converted with the BT.601 luma weights; the viewer dithers
 * that grayscale down to 1 bpp for the e-ink panel.
 *
 * Loading the file up front (rather than streaming from SD during decode) keeps
 * the shared SPI lock held only for the read, so decoding never blocks the
 * display flush.
 *
 * Return value: 0 on success, negative on failure (see IMG_ERR_*).
 * On success *out is malloc'd and owned by the caller (free() it).
 */
#ifndef __IMG_CODEC_H__
#define __IMG_CODEC_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IMG_ERR_FORMAT   (-1)   /* not a valid file of this type      */
#define IMG_ERR_TOO_BIG  (-2)   /* exceeds the caller's pixel budget  */
#define IMG_ERR_DECODE   (-3)   /* codec reported an error            */
#define IMG_ERR_MEMORY   (-4)   /* out of memory                      */
#define IMG_ERR_UNSUPP   (-5)   /* valid but unsupported variant      */

/* BT.601 luma, integer weights summing to 256. */
#define IMG_LUMA(r, g, b) ((uint8_t)(((r) * 77 + (g) * 150 + (b) * 29) >> 8))

int img_png_decode_gray(const uint8_t *data, size_t len,
                        uint32_t max_pixels,
                        uint8_t **out, int *w, int *h);

/* JPEG can descale by 1/2, 1/4 or 1/8 while decoding, so a large photo is
 * reduced to fit max_pixels instead of being rejected. */
int img_jpg_decode_gray(const uint8_t *data, size_t len,
                        uint32_t max_pixels,
                        uint8_t **out, int *w, int *h);

/* Uncompressed BMP: 1/4/8-bit palette, 24-bit and 32-bit, top-down or bottom-up. */
int img_bmp_decode_gray(const uint8_t *data, size_t len,
                        uint32_t max_pixels,
                        uint8_t **out, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* __IMG_CODEC_H__ */
