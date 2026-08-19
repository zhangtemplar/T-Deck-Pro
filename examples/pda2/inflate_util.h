/**
 * @file      inflate_util.h
 * @brief     Raw DEFLATE decompression, shared by the image and EPUB code.
 *
 * ZIP entries (compression method 8) are bare DEFLATE with no zlib wrapper,
 * which is exactly what lodepng's inflate consumes — so EPUB support needs no
 * new decompressor. The implementation lives in img_codec_png.c because that is
 * the only translation unit that compiles lodepng.
 */
#ifndef __INFLATE_UTIL_H__
#define __INFLATE_UTIL_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Inflate raw DEFLATE data. On success *out is malloc'd (caller frees) and
 * *out_len is its size. Returns 0 on success, negative on failure. */
int inflate_raw(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* __INFLATE_UTIL_H__ */
