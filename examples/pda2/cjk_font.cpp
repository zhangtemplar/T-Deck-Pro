/**
 * @file      cjk_font.cpp
 * @brief     Fast CJK font: pre-rasterized bitmaps cached in PSRAM (see cjk_font.h).
 *
 * Runtime TTF rasterization is far too slow for CJK on this MCU (the bottleneck
 * is stb_truetype rendering, not I/O — even with the whole font in PSRAM a
 * screen of Chinese took minutes). So the glyphs are rasterized ONCE, offline,
 * at a single pixel size (tools/rasterize_cjk_font.py) into a flat binary blob.
 * At boot we load that blob straight into PSRAM and expose it to LVGL as a
 * custom font whose glyph bitmaps are already rendered — blitting is instant,
 * with no rasterization at runtime.
 *
 * A streaming TTF (via a minimal SD filesystem driver, letter 'S') is used only
 * when no raster blob is present. It is NOT chained behind the raster: drawing
 * one glyph through it costs about a second, so a single rare character in a
 * dictionary entry or an EPUB page would stall the UI. With a raster loaded,
 * characters outside it draw as placeholder boxes instead.
 */
#include "cjk_font.h"
#include <Arduino.h>
#include <SD.h>
#include <string.h>
#include <esp_heap_caps.h>
#include "utilities.h"      /* BOARD_SD_CS */

extern void shared_spi_lock(void);
extern void shared_spi_unlock(void);
extern void shared_spi_prepare_device(int cs_pin);

/* Pre-rasterized bitmap fonts (produced by tools/rasterize_cjk_font.py).
 * cjk_14 matches the Montserrat-14 body font; cjk_16 is a spare/fallback. */
#define CJK_RASTER_PATH_14 "/fonts/cjk_14.bin"
#define CJK_RASTER_PATH_16 "/fonts/cjk_16.bin"

/* Streaming TTF fallback: same path/filename used by LilyGoLib/examples/pda. */
#define CJK_TTF_PATH   "/fonts/dict_font.ttf"
#define CJK_FS_LETTER  'S'
#define CJK_TTF_SIZE   16

/* Rendered-glyph LRU cache for the streaming fallback (PSRAM, via LVGL pool). */
#define CJK_GLYPH_CACHE   (128 * 1024)

/* Streaming-TTF fallback for glyphs outside the rasterized set.
 *
 * OFF whenever a raster blob is available. Rasterizing a glyph at runtime
 * costs on the order of a second — that cost is the whole reason the offline
 * raster exists — so a single rare character in a dictionary entry or an EPUB
 * page stalls rendering for as long as it takes to draw, which looks exactly
 * like a hung device. An empty placeholder box is a far better failure mode.
 *
 * The raster covers CJK Unified (0x4E00-0x9FFF), kana and punctuation but not
 * Extension A; widen it with tools/rasterize_cjk_font.py --ext-a rather than
 * turning this back on. */
#define CJK_ENABLE_TTF_FALLBACK 0

/* Whole-TTF-in-PSRAM threshold (only used when no pre-rasterized blob exists). */
#define CJK_PSRAM_HEADROOM (1024 * 1024)

lv_font_t g_font_cn;
lv_font_t g_font_cn_large;
static lv_font_t *s_cjk_ttf = NULL;

/* --------------------- LVGL FS driver over Arduino SD --------------------- */
/* LVGL strips the "S:" prefix, so callbacks receive a plain path like
 * "/fonts/dict_font.ttf". Every access takes the shared-SPI lock and selects the
 * SD card; the recursive mutex makes nested locking safe. Each open file carries
 * a read-ahead buffer so stb_truetype's many tiny reads hit RAM, not the SD. */
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
    return LV_FS_RES_OK;
}

static lv_fs_res_t sd_tell_cb(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p)
{
    LV_UNUSED(drv);
    sd_file_t *sf = (sd_file_t *)file_p;
    *pos_p = sf->pos;
    return LV_FS_RES_OK;
}

/* ---------------- Pre-rasterized bitmap font (PSRAM-resident) ------------- */
/* On-disk layout produced by tools/rasterize_cjk_font.py; loaded verbatim into
 * one PSRAM buffer. See that script's header for the byte-exact format. */

typedef struct __attribute__((packed)) {
    char     magic[4];        /* "CJK1" */
    uint16_t version;         /* 1 */
    uint8_t  bpp;             /* 1 */
    uint8_t  flags;
    int16_t  px_size;
    int16_t  line_height;
    int16_t  base_line;
    uint16_t reserved;
    uint32_t glyph_count;
    uint32_t codepoints_off;
    uint32_t glyphs_off;
    uint32_t bitmap_off;
    uint32_t bitmap_size;
} cjk_raster_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t bmp_off;         /* offset into the bitmap blob */
    uint16_t adv_w;           /* advance width, whole px */
    uint8_t  box_w;
    uint8_t  box_h;
    int8_t   ofs_x;
    int8_t   ofs_y;
} cjk_raster_glyph_t;

typedef struct {
    uint8_t                  *blob;      /* whole file, kept for the font's life */
    const uint32_t           *cps;       /* sorted codepoint table */
    const cjk_raster_glyph_t *glyphs;    /* parallel glyph descriptors */
    const uint8_t            *bitmaps;   /* bitmap blob base */
    uint32_t                  count;
    uint32_t                  last_letter;
    int32_t                   last_idx;  /* cache: dsc+bitmap are called in pairs */
} cjk_raster_ctx_t;

static cjk_raster_ctx_t s_raster_ctx_14, s_raster_ctx_16;
static lv_font_t        s_raster_font_14, s_raster_font_16;

static int32_t cjk_raster_find(cjk_raster_ctx_t *ctx, uint32_t letter)
{
    if (letter == ctx->last_letter) return ctx->last_idx;
    int lo = 0, hi = (int)ctx->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t c = ctx->cps[mid];
        if (c == letter) {
            ctx->last_letter = letter;
            ctx->last_idx = mid;
            return mid;
        }
        if (c < letter) lo = mid + 1;
        else            hi = mid - 1;
    }
    ctx->last_letter = letter;    /* cache the miss too */
    ctx->last_idx = -1;
    return -1;
}

static bool cjk_raster_get_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc,
                               uint32_t letter, uint32_t letter_next)
{
    LV_UNUSED(letter_next);
    cjk_raster_ctx_t *ctx = (cjk_raster_ctx_t *)font->dsc;
    int32_t idx = cjk_raster_find(ctx, letter);
    if (idx < 0) return false;
    const cjk_raster_glyph_t *g = &ctx->glyphs[idx];
    dsc->adv_w = g->adv_w;
    dsc->box_w = g->box_w;
    dsc->box_h = g->box_h;
    dsc->ofs_x = g->ofs_x;
    dsc->ofs_y = g->ofs_y;
    dsc->bpp   = 1;
    dsc->is_placeholder = 0;
    return true;
}

static const uint8_t *cjk_raster_get_bitmap(const lv_font_t *font, uint32_t letter)
{
    cjk_raster_ctx_t *ctx = (cjk_raster_ctx_t *)font->dsc;
    int32_t idx = cjk_raster_find(ctx, letter);
    if (idx < 0) return NULL;
    return ctx->bitmaps + ctx->glyphs[idx].bmp_off;
}

/* Load a pre-rasterized blob into PSRAM and wire up the caller's lv_font_t/ctx.
 * Returns true on success, false if the file is missing/invalid. */
static bool cjk_load_raster(const char *path, cjk_raster_ctx_t *ctx, lv_font_t *font)
{
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    File f = SD.open(path, FILE_READ);
    if (!f) { shared_spi_unlock(); return false; }
    size_t sz = f.size();
    if (sz < sizeof(cjk_raster_hdr_t)) { f.close(); shared_spi_unlock(); return false; }

    uint8_t *blob = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (!blob) { f.close(); shared_spi_unlock(); Serial.printf("[CJK] raster %s: PSRAM alloc failed\n", path); return false; }

    f.seek(0);
    size_t got = 0;
    while (got < sz) {
        size_t want = sz - got;
        if (want > 4096) want = 4096;
        int n = f.read(blob + got, want);
        if (n <= 0) break;
        got += (size_t)n;
    }
    f.close();
    shared_spi_unlock();

    if (got != sz) { heap_caps_free(blob); Serial.printf("[CJK] raster %s: read incomplete\n", path); return false; }

    cjk_raster_hdr_t *h = (cjk_raster_hdr_t *)blob;
    if (memcmp(h->magic, "CJK1", 4) != 0 || h->version != 1 || h->bpp != 1) {
        heap_caps_free(blob);
        Serial.printf("[CJK] raster %s: bad magic/version\n", path);
        return false;
    }
    /* Bounds-check the three tables against the file size. */
    uint64_t cp_end  = (uint64_t)h->codepoints_off + (uint64_t)h->glyph_count * 4;
    uint64_t gl_end  = (uint64_t)h->glyphs_off + (uint64_t)h->glyph_count * sizeof(cjk_raster_glyph_t);
    uint64_t bm_end  = (uint64_t)h->bitmap_off + (uint64_t)h->bitmap_size;
    if (h->glyph_count == 0 || cp_end > sz || gl_end > sz || bm_end > sz) {
        heap_caps_free(blob);
        Serial.printf("[CJK] raster %s: table bounds invalid\n", path);
        return false;
    }

    ctx->blob        = blob;
    ctx->cps         = (const uint32_t *)(blob + h->codepoints_off);
    ctx->glyphs      = (const cjk_raster_glyph_t *)(blob + h->glyphs_off);
    ctx->bitmaps     = blob + h->bitmap_off;
    ctx->count       = h->glyph_count;
    ctx->last_letter = 0xFFFFFFFFu;
    ctx->last_idx    = -1;

    memset(font, 0, sizeof(*font));
    font->get_glyph_dsc    = cjk_raster_get_dsc;
    font->get_glyph_bitmap = cjk_raster_get_bitmap;
    font->line_height      = h->line_height;
    font->base_line        = h->base_line;
    font->subpx            = LV_FONT_SUBPX_NONE;
    font->dsc              = ctx;
    font->fallback         = NULL;

    Serial.printf("[CJK] raster %s: %u glyphs, %u KB in PSRAM (size=%dpx)\n",
                  path, (unsigned)h->glyph_count, (unsigned)(sz / 1024), (int)h->px_size);
    return true;
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

    /* Base UI font first, so &g_font_cn is always valid even without a CJK font
     * (then Chinese simply won't render, but nothing breaks). */
    g_font_cn = lv_font_montserrat_14;
    g_font_cn.fallback = NULL;

    /* 1) Preferred: pre-rasterized bitmaps in PSRAM (instant, no rasterization).
     *    cjk_14 is the body-matched size; cjk_16 is a spare/fallback. Either may
     *    be absent — whatever's present is used. */
    lv_font_t *r14 = cjk_load_raster(CJK_RASTER_PATH_14, &s_raster_ctx_14, &s_raster_font_14)
                     ? &s_raster_font_14 : NULL;
    lv_font_t *r16 = cjk_load_raster(CJK_RASTER_PATH_16, &s_raster_ctx_16, &s_raster_font_16)
                     ? &s_raster_font_16 : NULL;
    bool have_raster = (r14 || r16);

    /* 2) TTF fallback. With a raster font present, keep it light (streaming) so a
     *    rare missing glyph still renders (slowly). Without one, fall back to the
     *    old strategy: whole TTF in PSRAM if it fits, else streaming. */
    if (!have_raster) {
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t fsize = 0;
        shared_spi_lock();
        shared_spi_prepare_device(BOARD_SD_CS);
        File f = SD.open(CJK_TTF_PATH, FILE_READ);
        if (f) fsize = f.size();
        Serial.printf("[CJK] no raster blob; TTF fallback. PSRAM free=%u KB, font=%u KB\n",
                      (unsigned)(free_psram / 1024), (unsigned)(fsize / 1024));

        if (fsize > 0 && (fsize + CJK_PSRAM_HEADROOM) < free_psram) {
            uint8_t *font_buf = (uint8_t *)heap_caps_malloc(fsize, MALLOC_CAP_SPIRAM);
            if (font_buf && f) {
                size_t got = 0;
                f.seek(0);
                while (got < fsize) {
                    size_t want = fsize - got;
                    if (want > 4096) want = 4096;
                    int n = f.read(font_buf + got, want);
                    if (n <= 0) break;
                    got += (size_t)n;
                }
                if (got == fsize) {
                    s_cjk_ttf = lv_tiny_ttf_create_data_ex(font_buf, fsize, CJK_TTF_SIZE, CJK_GLYPH_CACHE);
                    Serial.println("[CJK] whole TTF cached in PSRAM");
                } else {
                    heap_caps_free(font_buf);
                }
            } else if (font_buf) {
                heap_caps_free(font_buf);
            }
        }
        if (f) f.close();
        shared_spi_unlock();
    }

    /* Only fall back to the TTF when there is no raster at all; with a raster
     * present the runtime rasterization cost is not worth the rare glyph, and
     * skipping it also returns CJK_GLYPH_CACHE to the LVGL pool. */
    if (!s_cjk_ttf && (!have_raster || CJK_ENABLE_TTF_FALLBACK)) {
        s_cjk_ttf = lv_tiny_ttf_create_file_ex("S:" CJK_TTF_PATH, CJK_TTF_SIZE, CJK_GLYPH_CACHE);
        if (s_cjk_ttf) Serial.println("[CJK] streaming TTF fallback ready");
    } else if (have_raster) {
        Serial.println("[CJK] raster only; rare glyphs draw as boxes "
                       "(runtime TTF rasterization would stall the UI)");
    }

    /* 3) Build the glyph-source chain and install it as the theme font.
     *    montserrat (ASCII) -> cjk_14 -> cjk_16 -> TTF (rare CJK).
     *    s_cjk_ttf may be NULL if the TTF is absent; that just ends the chain. */
    lv_font_t *cjk_primary;
    if (r14) {
        r14->fallback = r16 ? r16 : s_cjk_ttf;
        if (r16) r16->fallback = s_cjk_ttf;
        cjk_primary = r14;
    } else if (r16) {
        r16->fallback = s_cjk_ttf;
        cjk_primary = r16;
    } else {
        cjk_primary = s_cjk_ttf;
    }

    /* A second, larger composition for the reader's big-text mode: montserrat 18
     * over the 16px CJK raster (which already chains on to the TTF). */
    g_font_cn_large = lv_font_montserrat_18;
    g_font_cn_large.fallback = r16 ? r16 : cjk_primary;

    if (cjk_primary) {
        g_font_cn.fallback = cjk_primary;
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
