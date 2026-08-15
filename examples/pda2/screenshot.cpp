/**
 * @file      screenshot.cpp
 * @brief     Dump the e-ink framebuffer to a 1-bpp BMP on the SD card.
 *
 * The LVGL port renders in full_refresh mode into `decodebuffer`, a full-screen
 * 1-bpp bitmap (see factory.ino: convert_lvgl_buf_to_epd_bitmap). Bit = 1 means
 * a white pixel, bit = 0 means black, MSB = leftmost pixel — which maps directly
 * onto a 1-bpp BMP with palette {0: black, 1: white}. We just repackage the same
 * bytes with a BMP header (rows bottom-up, padded to 4 bytes) and write to SD.
 */
#include "screenshot.h"
#include <Arduino.h>
#include <SD.h>
#include <time.h>
#include "utilities.h"      /* LCD_HOR_SIZE, LCD_VER_SIZE, BOARD_SD_CS */

extern uint8_t *decodebuffer;
extern void shared_spi_lock(void);
extern void shared_spi_unlock(void);
extern void shared_spi_prepare_device(int cs_pin);

#define SHOT_DIR "/images"

static inline void put_u16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static inline void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

bool screenshot_capture(void)
{
    if (!decodebuffer) return false;

    const int w = LCD_HOR_SIZE;                 /* 240 */
    const int h = LCD_VER_SIZE;                 /* 320 */
    const int src_stride = (w + 7) / 8;         /* 30 bytes/row in decodebuffer  */
    const int row_size   = ((w + 31) / 32) * 4; /* 32 bytes/row in BMP (4-aligned)*/
    const uint32_t img_size  = (uint32_t)row_size * h;
    const uint32_t data_off  = 14 + 40 + 2 * 4; /* headers + 2-colour palette     */
    const uint32_t file_size = data_off + img_size;

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);

    if (!SD.exists(SHOT_DIR)) {
        SD.mkdir(SHOT_DIR);
    }

    /* Filename from local time; disambiguate with a suffix if it already exists
     * (e.g. multiple shots within the same second, or an unset clock). */
    char path[96];
    time_t now; time(&now);
    struct tm tm; localtime_r(&now, &tm);
    snprintf(path, sizeof(path), SHOT_DIR "/screenshot_%04d%02d%02d_%02d%02d%02d.bmp",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    for (int n = 1; SD.exists(path) && n < 100; ++n) {
        snprintf(path, sizeof(path), SHOT_DIR "/screenshot_%04d%02d%02d_%02d%02d%02d_%d.bmp",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, n);
    }

    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        shared_spi_unlock();
        Serial.printf("[SHOT] open '%s' failed\n", path);
        return false;
    }

    /* BMP file header (14) + info header (40) + palette (8). */
    uint8_t hdr[14 + 40 + 8];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    put_u32(hdr + 2,  file_size);
    put_u32(hdr + 10, data_off);
    put_u32(hdr + 14, 40);          /* info header size            */
    put_u32(hdr + 18, w);           /* width                       */
    put_u32(hdr + 22, h);           /* height (positive = bottom-up)*/
    put_u16(hdr + 26, 1);           /* planes                      */
    put_u16(hdr + 28, 1);           /* bits per pixel              */
    put_u32(hdr + 34, img_size);    /* image size                  */
    put_u32(hdr + 46, 2);           /* colours used                */
    /* Palette (BGRA): index 0 = black, index 1 = white. */
    hdr[54] = 0x00; hdr[55] = 0x00; hdr[56] = 0x00; hdr[57] = 0x00;
    hdr[58] = 0xFF; hdr[59] = 0xFF; hdr[60] = 0xFF; hdr[61] = 0x00;
    f.write(hdr, sizeof(hdr));

    /* Pixel rows: BMP is bottom-up, so emit the last decodebuffer row first.
     * Copy the 30 valid bytes and zero-pad to the 32-byte BMP row. */
    uint8_t row[64];                 /* >= row_size */
    memset(row, 0, sizeof(row));
    bool ok = true;
    for (int y = h - 1; y >= 0; --y) {
        memcpy(row, decodebuffer + (size_t)y * src_stride, src_stride);
        if (f.write(row, row_size) != (size_t)row_size) { ok = false; break; }
    }

    f.close();
    shared_spi_unlock();

    if (ok) Serial.printf("[SHOT] screenshot saved to SD: %s (%ux%u)\n", path, w, h);
    else    Serial.printf("[SHOT] write failed: %s\n", path);
    return ok;
}
