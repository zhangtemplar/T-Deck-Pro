/**
 * @file      cjk_font.cpp
 * @brief     Streaming CJK font from SD (see cjk_font.h).
 *
 * Provides a minimal LVGL filesystem driver (letter 'S') over the Arduino SD
 * library, guarded by the project's shared-SPI lock so on-demand glyph reads
 * coexist with the e-ink panel on the same SPI bus. TinyTTF then streams glyphs
 * from the TTF instead of loading it whole into PSRAM.
 */
#include "cjk_font.h"
#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include "utilities.h"      /* BOARD_SD_CS */

extern void shared_spi_lock(void);
extern void shared_spi_unlock(void);
extern void shared_spi_prepare_device(int cs_pin);

/* Path of the CJK TTF on the SD card, and render size. Same path/filename used
 * by LilyGoLib/examples/pda (ui_dictionary.cpp). */
#define CJK_TTF_PATH   "/fonts/dict_font.ttf"
#define CJK_FS_LETTER  'S'
#define CJK_TTF_SIZE   16

/* Rendered-glyph LRU cache (PSRAM, via the LVGL pool). ~256 B/glyph at 16px, so
 * 128 KB caches ~500 glyphs — a whole screen of Chinese stays cached, avoiding
 * re-rasterization (and re-reads, in the streaming fallback). */
#define CJK_GLYPH_CACHE   (128 * 1024)

/* Load the whole TTF into PSRAM (no per-glyph SD reads at all) when it fits with
 * this much headroom left free; otherwise stream it from SD. */
#define CJK_PSRAM_HEADROOM (1024 * 1024)

/* Compiled bitmap CJK font in flash: glyphs are pre-rasterized, so common
 * Chinese renders instantly (no SD, no rasterization). Generate src/font_cjk_16.c
 * with lv_font_conv (see notes), drop it in, then set this to 1. The streaming
 * TTF is kept as a deep fallback for rare characters outside the compiled set. */
#define CJK_HAVE_COMPILED 0
#if CJK_HAVE_COMPILED
LV_FONT_DECLARE(font_cjk_16)
static lv_font_t g_font_cjk;   /* compiled font + TTF fallback for rare chars */
#endif

lv_font_t g_font_cn;
static lv_font_t *s_cjk_ttf = NULL;

/* --------------------- LVGL FS driver over Arduino SD --------------------- */
/* LVGL strips the "S:" prefix, so callbacks receive a plain path like
 * "/fonts/cjk.ttf". Every access takes the shared-SPI lock and selects the SD
 * card; the recursive mutex makes nested locking safe. */

/* stb_truetype reads the font in a huge number of tiny (2-byte) chunks. Hitting
 * the SD card for each one (lock + CS churn + SPI transaction) is unusably slow
 * (~9000 reads to render one CJK glyph). So each open file carries a read-ahead
 * buffer: reads are served from RAM and the SD is only touched in BUF-sized
 * chunks, cutting thousands of SD transactions down to a few dozen. */
/* Read-ahead buffer for the streaming fallback. stb reads scattered, mostly
 * small runs, so a very large buffer mainly wastes transfer time on seeks; 4 KB
 * captures a typical glyph outline (and loca/hmtx) in one SD transaction.
 * (Unused when the whole font fits in PSRAM — the common case here.) */
#define SD_FS_BUF 4096

struct sd_file_t {
    File     f;
    uint32_t pos;        /* logical read position */
    uint32_t buf_start;  /* file offset of buf[0] */
    uint32_t buf_len;    /* valid bytes in buf */
    uint8_t  buf[SD_FS_BUF];
};

static void *sd_open_cb(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
    LV_UNUSED(drv);
    if (mode != LV_FS_MODE_RD) return NULL;
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    File f = SD.open(path, FILE_READ);
    shared_spi_unlock();
    Serial.printf("[fs] open '%s' -> %s\n", path, f ? "ok" : "FAIL");
    if (!f) return NULL;
    sd_file_t *sf = new sd_file_t();
    sf->f = f;
    sf->pos = 0;
    sf->buf_start = 0;
    sf->buf_len = 0;
    return sf;
}

static lv_fs_res_t sd_close_cb(lv_fs_drv_t *drv, void *file_p)
{
    LV_UNUSED(drv);
    sd_file_t *sf = (sd_file_t *)file_p;
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    sf->f.close();
    shared_spi_unlock();
    delete sf;
    return LV_FS_RES_OK;
}

static lv_fs_res_t sd_read_cb(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br)
{
    LV_UNUSED(drv);
    sd_file_t *sf = (sd_file_t *)file_p;
    uint8_t *out = (uint8_t *)buf;
    uint32_t total = 0;

    while (total < btr) {
        /* Refill the buffer if pos is outside it. */
        if (sf->pos < sf->buf_start || sf->pos >= sf->buf_start + sf->buf_len) {
            shared_spi_lock();
            shared_spi_prepare_device(BOARD_SD_CS);
            sf->f.seek(sf->pos);
            int n = sf->f.read(sf->buf, SD_FS_BUF);
            shared_spi_unlock();
            if (n <= 0) break;                 /* EOF or error */
            sf->buf_start = sf->pos;
            sf->buf_len = (uint32_t)n;
        }
        uint32_t off = sf->pos - sf->buf_start;
        uint32_t avail = sf->buf_len - off;
        uint32_t chunk = btr - total;
        if (chunk > avail) chunk = avail;
        memcpy(out + total, sf->buf + off, chunk);
        sf->pos += chunk;
        total += chunk;
    }

    *br = total;
    return LV_FS_RES_OK;
}

static lv_fs_res_t sd_seek_cb(lv_fs_drv_t *drv, void *file_p, uint32_t pos, lv_fs_whence_t whence)
{
    LV_UNUSED(drv);
    sd_file_t *sf = (sd_file_t *)file_p;
    if (whence == LV_FS_SEEK_CUR)      sf->pos += pos;
    else if (whence == LV_FS_SEEK_END) sf->pos = sf->f.size() + pos;
    else                               sf->pos = pos;   /* LV_FS_SEEK_SET */
    /* Buffer is validated lazily on the next read. */
    return LV_FS_RES_OK;
}

static lv_fs_res_t sd_tell_cb(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p)
{
    LV_UNUSED(drv);
    sd_file_t *sf = (sd_file_t *)file_p;
    *pos_p = sf->pos;
    return LV_FS_RES_OK;
}

/* ------------------------------- init ------------------------------------ */

void cjk_font_init(void)
{
    static lv_fs_drv_t drv;
    lv_fs_drv_init(&drv);
    drv.letter   = CJK_FS_LETTER;
    drv.open_cb  = sd_open_cb;
    drv.close_cb = sd_close_cb;
    drv.read_cb  = sd_read_cb;
    drv.seek_cb  = sd_seek_cb;
    drv.tell_cb  = sd_tell_cb;
    lv_fs_drv_register(&drv);

    /* Base UI font first, so &g_font_cn is always valid even if the TTF is
     * missing (then Chinese simply won't render, but nothing breaks). */
    g_font_cn = lv_font_montserrat_14;
    g_font_cn.fallback = NULL;

    /* Inspect the font file size and free PSRAM to decide the load strategy. */
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t fsize = 0;
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    File f = SD.open(CJK_TTF_PATH, FILE_READ);
    if (f) fsize = f.size();
    shared_spi_unlock();
    Serial.printf("[CJK] PSRAM free=%u KB, font=%u KB\n",
                  (unsigned)(free_psram / 1024), (unsigned)(fsize / 1024));

    /* Preferred: load the whole TTF into PSRAM once -> zero SD reads while
     * rendering. Falls back to buffered streaming if it wouldn't fit. */
    if (fsize > 0 && (fsize + CJK_PSRAM_HEADROOM) < free_psram) {
        uint8_t *font_buf = (uint8_t *)heap_caps_malloc(fsize, MALLOC_CAP_SPIRAM);
        if (font_buf && f) {
            size_t got = 0;
            shared_spi_lock();
            shared_spi_prepare_device(BOARD_SD_CS);
            f.seek(0);
            while (got < fsize) {
                size_t want = fsize - got;
                if (want > 4096) want = 4096;
                int n = f.read(font_buf + got, want);
                if (n <= 0) break;
                got += (size_t)n;
            }
            shared_spi_unlock();
            if (got == fsize) {
                /* font_buf is intentionally kept for the font's lifetime. */
                s_cjk_ttf = lv_tiny_ttf_create_data_ex(font_buf, fsize, CJK_TTF_SIZE, CJK_GLYPH_CACHE);
                Serial.println("[CJK] whole font cached in PSRAM (no SD reads while rendering)");
            } else {
                heap_caps_free(font_buf);
                Serial.println("[CJK] font read incomplete, will stream");
            }
        } else if (font_buf) {
            heap_caps_free(font_buf);
        }
    }
    if (f) f.close();

    /* Streaming fallback (large glyph cache still keeps re-renders cheap). */
    if (!s_cjk_ttf) {
        s_cjk_ttf = lv_tiny_ttf_create_file_ex("S:" CJK_TTF_PATH, CJK_TTF_SIZE, CJK_GLYPH_CACHE);
        if (s_cjk_ttf) Serial.println("[CJK] streaming TTF from SD (buffered) with glyph cache");
    }

    /* CJK glyph source: prefer the compiled flash bitmap font (instant), with
     * the TTF as a deep fallback for rare characters. */
    lv_font_t *cjk_primary = s_cjk_ttf;
#if CJK_HAVE_COMPILED
    g_font_cjk = font_cjk_16;          /* compiled common-CJK bitmaps (flash) */
    g_font_cjk.fallback = s_cjk_ttf;   /* rare chars -> TTF (may be NULL) */
    cjk_primary = &g_font_cjk;
    Serial.println("[CJK] compiled bitmap font active (+ TTF fallback for rare chars)");
#endif

    if (cjk_primary) {
        g_font_cn.fallback = cjk_primary;
        /* Install as the theme font so all default-font labels gain CJK. */
        lv_disp_t *disp = lv_disp_get_default();
        lv_theme_t *th = lv_theme_default_init(disp,
                                               lv_palette_main(LV_PALETTE_BLUE),
                                               lv_palette_main(LV_PALETTE_RED),
                                               LV_THEME_DEFAULT_DARK, &g_font_cn);
        lv_disp_set_theme(disp, th);
    } else {
        Serial.println("[CJK] no CJK font available — Chinese will not render");
    }
}
