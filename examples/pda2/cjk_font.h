/**
 * @file      cjk_font.h
 * @brief     App-wide Chinese (CJK) text support via a streaming TTF on SD.
 *
 * A TTF on the SD card is opened through a custom LVGL filesystem driver and
 * rendered with TinyTTF's file-streaming mode, so only a small glyph cache lives
 * in PSRAM (a complete CJK TTF never has to fit in RAM). The loaded font is set
 * as the fallback of the default UI font and installed as the theme font, so
 * Chinese renders app-wide.
 */
#ifndef CJK_FONT_H
#define CJK_FONT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/* Default UI font (montserrat) with the SD CJK TTF as its glyph fallback.
 * Valid after cjk_font_init(); safe to reference before then (plain UI font,
 * no CJK) since it is populated during init, before any screen is created. */
extern lv_font_t g_font_cn;

/* Larger composition (montserrat 18 over the 16px CJK raster) for the reader's
 * big-text mode. Same validity rules as g_font_cn. */
extern lv_font_t g_font_cn_large;

/**
 * @brief Register the SD filesystem driver, load the CJK TTF, build g_font_cn,
 *        and install it as the theme font. Call once after lvgl_init() and after
 *        the SD card is mounted, before any UI screen is created.
 */
void cjk_font_init(void);

#ifdef __cplusplus
}
#endif

#endif /* CJK_FONT_H */
