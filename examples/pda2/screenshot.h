/**
 * @file      screenshot.h
 * @brief     Capture the current e-ink framebuffer to an SD-card image.
 *
 * The display port keeps the whole screen as a 1-bpp bitmap in the global
 * `decodebuffer` (full_refresh mode), so a screenshot is just that buffer
 * written out as a monochrome BMP. Triggered from the keypad by Alt + P.
 */
#ifndef __SCREENSHOT_H__
#define __SCREENSHOT_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Save the current screen to /images/screenshot_YYYYMMDD_HHMMSS.bmp on the SD
 * card (the /images folder is created if it doesn't exist). Returns true on
 * success. Safe to call from the main loop (keypad handler). */
bool screenshot_capture(void);

#ifdef __cplusplus
}
#endif

#endif /* __SCREENSHOT_H__ */
