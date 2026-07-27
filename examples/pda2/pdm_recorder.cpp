/**
 * @file      pdm_recorder.cpp
 * @brief     PDM mic recorder. Two paths:
 *              1) Legacy pdm_record_wav() — fixed duration, WAV in PSRAM (used by Voice AI).
 *              2) pdm_record_start/stop() — streaming Opus encode to SD; pdm_play_start/stop()
 *                 streams the same file back via Opus decode → I2S TX.
 *
 * SD writes go through shared_spi_lock() because the SPI bus is shared with EPD/LoRa.
 */
#include "pdm_recorder.h"
#include "utilities.h"
#include "factory.h"
#include "opus.h"
#include "FS.h"
#include "SD.h"
#include <Arduino.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <string.h>

#define I2S_PORT          I2S_NUM_0
#define MAGIC             "OPUS"
#define HDR_BYTES         20
#define HDR_VERSION       2
#define FRAME_MS          20
#define DEFAULT_BITRATE   24000
#define DEFAULT_COMPLEX   0    /* fastest encode; VOIP at 16k is fine at 0 */

static bool i2s_installed = false;

/* ---------- I2S helpers ---------- */
static bool pdm_i2s_rx_install(int sample_rate)
{
    /* Always uninstall — Audio library or a prior call may hold I2S_NUM_0 */
    i2s_driver_uninstall(I2S_PORT);
    i2s_installed = false;
    i2s_config_t i2s_config = {};
    i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
    i2s_config.sample_rate = sample_rate;
    i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2s_config.dma_buf_count = 16;   /* 16×1024 = 16KB ≈ 512ms of headroom */
    i2s_config.dma_buf_len = 1024;
    i2s_config.use_apll = false;
    if (i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL) != ESP_OK) return false;
    i2s_pin_config_t pin_config = {};
    pin_config.bck_io_num = I2S_PIN_NO_CHANGE;
    pin_config.ws_io_num = BOARD_MIC_CLOCK;
    pin_config.data_out_num = I2S_PIN_NO_CHANGE;
    pin_config.data_in_num = BOARD_MIC_DATA;
    if (i2s_set_pin(I2S_PORT, &pin_config) != ESP_OK) {
        i2s_driver_uninstall(I2S_PORT);
        return false;
    }
    i2s_installed = true;
    return true;
}

static bool pdm_i2s_tx_install(int sample_rate)
{
    /* Always uninstall — Audio library or a prior call may hold I2S_NUM_0 */
    i2s_driver_uninstall(I2S_PORT);
    i2s_installed = false;
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = sample_rate;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 1024;
    cfg.use_apll = false;
    if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) return false;
    i2s_pin_config_t pins = {};
    pins.bck_io_num = BOARD_I2S_BCLK;
    pins.ws_io_num = BOARD_I2S_LRC;
    pins.data_out_num = BOARD_I2S_DOUT;
    pins.data_in_num = I2S_PIN_NO_CHANGE;
    if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
        i2s_driver_uninstall(I2S_PORT);
        return false;
    }
    i2s_installed = true;
    return true;
}

static void pdm_i2s_uninstall(void)
{
    if (i2s_installed) {
        i2s_driver_uninstall(I2S_PORT);
        i2s_installed = false;
    }
}

/* ---------- Legacy fixed-duration WAV path (kept for ui_voice_ai) ---------- */
static void write_wav_header(uint8_t *buf, int sample_rate, int num_samples)
{
    int data_size = num_samples * 2;
    int file_size = 44 + data_size - 8;
    memcpy(buf, "RIFF", 4);
    *(uint32_t *)(buf + 4) = file_size;
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    *(uint32_t *)(buf + 16) = 16;
    *(uint16_t *)(buf + 20) = 1;
    *(uint16_t *)(buf + 22) = 1;
    *(uint32_t *)(buf + 24) = sample_rate;
    *(uint32_t *)(buf + 28) = sample_rate * 2;
    *(uint16_t *)(buf + 32) = 2;
    *(uint16_t *)(buf + 34) = 16;
    memcpy(buf + 36, "data", 4);
    *(uint32_t *)(buf + 40) = data_size;
}

bool pdm_record_wav(int duration_sec, int sample_rate, uint8_t **wav_out, size_t *wav_len)
{
    *wav_out = NULL; *wav_len = 0;
    if (!pdm_i2s_rx_install(sample_rate)) return false;

    int total_samples = sample_rate * duration_sec;
    size_t total_bytes = total_samples * 2;
    size_t wav_size = 44 + total_bytes;
    uint8_t *wav = (uint8_t *)heap_caps_malloc(wav_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) {
        Serial.printf("[PDM] PSRAM alloc %zu failed\n", wav_size);
        pdm_i2s_uninstall();
        return false;
    }
    write_wav_header(wav, sample_rate, total_samples);
    uint8_t flush_buf[2048];
    size_t flush_read = 0;
    i2s_read(I2S_PORT, flush_buf, sizeof(flush_buf), &flush_read, portMAX_DELAY);

    size_t offset = 44;
    size_t remaining = total_bytes;
    while (remaining > 0) {
        size_t to_read = remaining > 4096 ? 4096 : remaining;
        size_t bytes_read = 0;
        esp_err_t err = i2s_read(I2S_PORT, wav + offset, to_read, &bytes_read, pdMS_TO_TICKS(1000));
        if (err != ESP_OK || bytes_read == 0) break;
        offset += bytes_read;
        remaining -= bytes_read;
    }
    pdm_i2s_uninstall();
    int actual_samples = (offset - 44) / 2;
    write_wav_header(wav, sample_rate, actual_samples);
    *wav_out = wav;
    *wav_len = offset;
    return true;
}

void pdm_restore_audio(void) { /* Audio library re-installs I2S on demand */ }

/* ---------- Streaming Opus recorder ---------- */
static TaskHandle_t   rec_task = NULL;
static volatile bool  rec_active = false;
static volatile bool  rec_stop_req = false;
static volatile int   rec_elapsed_sec = 0;

struct rec_args_t {
    char path[80];
    int  sample_rate;
    int  max_seconds;
};

static void rec_task_fn(void *arg)
{
    rec_args_t *a = (rec_args_t *)arg;
    int sample_rate = a->sample_rate;
    int max_seconds = a->max_seconds;
    char path[80];
    strncpy(path, a->path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    free(a);

    int frame_samples = sample_rate / 1000 * FRAME_MS;       /* 20ms */
    size_t pcm_bytes = frame_samples * sizeof(int16_t);
    uint8_t *pkt_buf = NULL;
    int16_t *pcm_buf = NULL;
    OpusEncoder *enc = NULL;
    File f;
    bool sd_locked = false;

    pcm_buf = (int16_t *)heap_caps_malloc(pcm_bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    pkt_buf = (uint8_t *)heap_caps_malloc(1024, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!pcm_buf || !pkt_buf) { Serial.println("[REC] alloc fail"); goto cleanup; }

    int err;
    enc = opus_encoder_create(sample_rate, 1, OPUS_APPLICATION_VOIP, &err);
    if (!enc || err != OPUS_OK) {
        Serial.printf("[REC] opus_encoder_create err=%d\n", err);
        goto cleanup;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(DEFAULT_BITRATE));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(DEFAULT_COMPLEX));

    if (!pdm_i2s_rx_install(sample_rate)) { Serial.println("[REC] i2s install fail"); goto cleanup; }

    /* flush noisy first samples */
    {
        size_t r = 0;
        i2s_read(I2S_PORT, pcm_buf, pcm_bytes, &r, portMAX_DELAY);
        i2s_read(I2S_PORT, pcm_buf, pcm_bytes, &r, portMAX_DELAY);
    }

    /* open file */
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    sd_locked = true;
    {
        const char *slash = strrchr(path, '/');
        if (slash) {
            char dir[80];
            size_t n = slash - path;
            if (n >= sizeof(dir)) n = sizeof(dir) - 1;
            memcpy(dir, path, n);
            dir[n] = '\0';
            if (dir[0] && !SD.exists(dir)) SD.mkdir(dir);
        }
        f = SD.open(path, FILE_WRITE);
    }
    if (!f) {
        shared_spi_unlock();
        sd_locked = false;
        Serial.printf("[REC] open %s failed\n", path);
        pdm_i2s_uninstall();
        goto cleanup;
    }

    /* write header (v2: total_frames at offset 16 patched on close) */
    {
        uint8_t hdr[HDR_BYTES] = {0};
        memcpy(hdr, MAGIC, 4);
        hdr[4] = HDR_VERSION;
        hdr[5] = 1;                                 /* channels */
        uint16_t fs16 = (uint16_t)frame_samples;
        uint32_t sr32 = (uint32_t)sample_rate;
        uint32_t br32 = (uint32_t)DEFAULT_BITRATE;
        memcpy(hdr + 6, &fs16, 2);
        memcpy(hdr + 8, &sr32, 4);
        memcpy(hdr + 12, &br32, 4);
        /* hdr[16..19] = total_frames, written later */
        f.write(hdr, HDR_BYTES);
    }
    shared_spi_unlock();
    sd_locked = false;

    {
        uint32_t start_ms = millis();
        uint32_t last_report_ms = start_ms;
        uint32_t last_report_frames = 0;
        int frames_per_sec = 1000 / FRAME_MS;
        uint32_t frame_idx = 0;
        uint64_t bytes_ok = 0;
        uint32_t write_errs = 0;
        rec_elapsed_sec = 0;

        while (!rec_stop_req) {
            size_t r = 0;
            esp_err_t e = i2s_read(I2S_PORT, pcm_buf, pcm_bytes, &r, pdMS_TO_TICKS(200));
            if (e != ESP_OK || r != pcm_bytes) continue;
            int pkt_len = opus_encode(enc, pcm_buf, frame_samples, pkt_buf, 1024);
            if (pkt_len < 0) {
                Serial.printf("[REC] opus_encode err=%d\n", pkt_len);
                break;
            }
            shared_spi_lock();
            shared_spi_prepare_device(BOARD_SD_CS);
            uint16_t pl = (uint16_t)pkt_len;
            size_t w1 = f.write((const uint8_t *)&pl, 2);
            size_t w2 = f.write(pkt_buf, pkt_len);
            shared_spi_unlock();
            if (w1 != 2 || w2 != (size_t)pkt_len) {
                write_errs++;
                if (write_errs <= 3)
                    Serial.printf("[REC] short write at frame %u: %u+%u/%d\n",
                                  (unsigned)frame_idx, (unsigned)w1, (unsigned)w2, pkt_len);
                if (write_errs > 10) {
                    Serial.println("[REC] too many write errors, stopping");
                    break;
                }
            } else {
                bytes_ok += 2 + pkt_len;
            }

            frame_idx++;
            rec_elapsed_sec = (int)((millis() - start_ms) / 1000);
            if (rec_elapsed_sec >= max_seconds) {
                Serial.printf("[REC] cap reached %ds\n", max_seconds);
                break;
            }
            /* periodic flush every ~1s */
            if ((frame_idx % frames_per_sec) == 0) {
                shared_spi_lock();
                shared_spi_prepare_device(BOARD_SD_CS);
                f.flush();
                shared_spi_unlock();
            }
            /* fps report every ~2s */
            uint32_t now = millis();
            if (now - last_report_ms >= 2000) {
                uint32_t dt = now - last_report_ms;
                uint32_t df = frame_idx - last_report_frames;
                uint32_t fps = (df * 1000) / (dt ? dt : 1);
                Serial.printf("[REC] fps=%u frames=%u errs=%u\n",
                              (unsigned)fps, (unsigned)frame_idx, (unsigned)write_errs);
                last_report_ms = now;
                last_report_frames = frame_idx;
            }
        }

        /* Trailer: EOF marker (2B zero) + total_frames (4B).
         * Kept at end-of-file so all writes are sequential — avoids the
         * seek+write header-patch pitfall on Arduino-ESP32 SD (FILE_WRITE
         * behaves as append on some versions). */
        shared_spi_lock();
        shared_spi_prepare_device(BOARD_SD_CS);
        uint16_t zero = 0;
        uint32_t total = frame_idx;
        f.write((const uint8_t *)&zero, 2);
        f.write((const uint8_t *)&total, 4);
        f.flush();
        size_t final_size = f.size();
        f.close();
        shared_spi_unlock();

        Serial.printf("[REC] frames=%u elapsed=%ds bytes_ok=%llu file_size=%u write_errs=%u\n",
                      (unsigned)frame_idx, rec_elapsed_sec,
                      (unsigned long long)bytes_ok, (unsigned)final_size, (unsigned)write_errs);
    }

    pdm_i2s_uninstall();

cleanup:
    if (sd_locked) shared_spi_unlock();
    if (enc) opus_encoder_destroy(enc);
    if (pcm_buf) free(pcm_buf);
    if (pkt_buf) free(pkt_buf);
    rec_active = false;
    rec_stop_req = false;
    rec_task = NULL;
    vTaskDelete(NULL);
}

bool pdm_record_start(const char *path, int sample_rate, int max_seconds)
{
    if (rec_active || rec_task) return false;
    rec_args_t *a = (rec_args_t *)malloc(sizeof(rec_args_t));
    if (!a) return false;
    strncpy(a->path, path, sizeof(a->path) - 1);
    a->path[sizeof(a->path) - 1] = '\0';
    a->sample_rate = sample_rate;
    a->max_seconds = max_seconds;
    rec_active = true;
    rec_stop_req = false;
    rec_elapsed_sec = 0;
    BaseType_t ok = xTaskCreatePinnedToCore(rec_task_fn, "opusRec", 32768, a, 5, &rec_task, 0);
    if (ok != pdPASS) {
        free(a);
        rec_active = false;
        rec_task = NULL;
        return false;
    }
    return true;
}

void pdm_record_stop(void) { rec_stop_req = true; }
bool pdm_record_is_active(void) { return rec_active; }
int  pdm_record_elapsed_sec(void) { return rec_elapsed_sec; }

/* ---------- Streaming Opus playback (Opus → I2S TX) ---------- */
static TaskHandle_t  play_task = NULL;
static volatile bool play_active = false;
static volatile bool play_stop_req = false;
static volatile int  play_total_frames = 0;
static volatile int  play_done_frames = 0;
static volatile int  play_frame_ms = FRAME_MS;
static volatile int  play_volume = 16;            /* 0..21 */

struct play_args_t { char path[80]; };

static void play_task_fn(void *arg)
{
    play_args_t *a = (play_args_t *)arg;
    char path[80];
    strncpy(path, a->path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    free(a);

    File f;
    OpusDecoder *dec = NULL;
    int16_t *pcm = NULL;
    uint8_t *pkt = NULL;
    int sample_rate = 16000;
    int frame_samples = 320;
    bool sd_locked = false;

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    sd_locked = true;
    f = SD.open(path, FILE_READ);
    if (!f) {
        shared_spi_unlock();
        sd_locked = false;
        Serial.printf("[PLAY] open %s failed\n", path);
        goto cleanup;
    }
    {
        uint8_t hdr[HDR_BYTES];
        if (f.read(hdr, HDR_BYTES) != HDR_BYTES || memcmp(hdr, MAGIC, 4) != 0) {
            shared_spi_unlock();
            sd_locked = false;
            f.close();
            Serial.println("[PLAY] bad header");
            goto cleanup;
        }
        uint16_t fs16;
        uint32_t sr32;
        memcpy(&fs16, hdr + 6, 2);
        memcpy(&sr32, hdr + 8, 4);
        frame_samples = (int)fs16;
        sample_rate = (int)sr32;
        /* Trailer format: last 4 bytes = uint32 total_frames.
         * Fall back to 0 (unknown) if file too small or seek fails. */
        uint32_t total = 0;
        size_t fsz = f.size();
        if (fsz >= HDR_BYTES + 4) {
            if (f.seek(fsz - 4)) {
                if (f.read((uint8_t *)&total, 4) != 4) total = 0;
            }
            f.seek(HDR_BYTES);
        }
        play_total_frames = (int)total;
        play_done_frames = 0;
        play_frame_ms = (sample_rate > 0 && frame_samples > 0)
                          ? (1000 * frame_samples / sample_rate) : FRAME_MS;
        Serial.printf("[PLAY] sr=%d fs=%d total_frames=%u frame_ms=%d\n",
                      sample_rate, frame_samples, (unsigned)total, play_frame_ms);
    }
    shared_spi_unlock();
    sd_locked = false;

    int err;
    dec = opus_decoder_create(sample_rate, 1, &err);
    if (!dec || err != OPUS_OK) {
        Serial.printf("[PLAY] decoder_create err=%d\n", err);
        f.close();
        goto cleanup;
    }
    /* max frame samples for output buffer headroom */
    pcm = (int16_t *)heap_caps_malloc(frame_samples * 2 * sizeof(int16_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    pkt = (uint8_t *)heap_caps_malloc(2048, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!pcm || !pkt) {
        Serial.println("[PLAY] alloc fail");
        f.close();
        goto cleanup;
    }

    if (!pdm_i2s_tx_install(sample_rate)) {
        Serial.println("[PLAY] i2s tx install fail");
        f.close();
        goto cleanup;
    }

    while (!play_stop_req) {
        uint16_t plen = 0;
        shared_spi_lock();
        shared_spi_prepare_device(BOARD_SD_CS);
        if (f.read((uint8_t *)&plen, 2) != 2) { shared_spi_unlock(); break; }
        if (plen == 0) { shared_spi_unlock(); break; }
        if (plen > 2048) { shared_spi_unlock(); break; }
        if (f.read(pkt, plen) != plen) { shared_spi_unlock(); break; }
        shared_spi_unlock();

        int n = opus_decode(dec, pkt, plen, pcm, frame_samples * 2, 0);
        if (n <= 0) continue;
        /* software volume scale (0..21 maps to 0..1) + mono → duplicate to L/R */
        int vol = play_volume; if (vol < 0) vol = 0; if (vol > 21) vol = 21;
        static int16_t stereo[1024];
        for (int i = 0; i < n && i < 1024; i++) {
            int32_t s = ((int32_t)pcm[i] * vol) / 21;
            stereo[i * 2]     = (int16_t)s;
            stereo[i * 2 + 1] = (int16_t)s;
        }
        size_t written = 0;
        i2s_write(I2S_PORT, stereo, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        play_done_frames++;
    }
    f.close();
    pdm_i2s_uninstall();

cleanup:
    if (sd_locked) shared_spi_unlock();
    if (dec) opus_decoder_destroy(dec);
    if (pcm) free(pcm);
    if (pkt) free(pkt);
    play_active = false;
    play_stop_req = false;
    play_task = NULL;
    vTaskDelete(NULL);
}

bool pdm_play_start(const char *path)
{
    if (play_active || play_task) return false;
    play_args_t *a = (play_args_t *)malloc(sizeof(play_args_t));
    if (!a) return false;
    strncpy(a->path, path, sizeof(a->path) - 1);
    a->path[sizeof(a->path) - 1] = '\0';
    play_active = true;
    play_stop_req = false;
    BaseType_t ok = xTaskCreatePinnedToCore(play_task_fn, "opusPlay", 32768, a, 5, &play_task, 0);
    if (ok != pdPASS) {
        free(a);
        play_active = false;
        play_task = NULL;
        return false;
    }
    return true;
}

void pdm_play_stop(void) { play_stop_req = true; }
bool pdm_play_is_active(void) { return play_active; }

int pdm_play_elapsed_sec(void) { return (play_done_frames * play_frame_ms) / 1000; }
int pdm_play_total_sec(void)   { return (play_total_frames * play_frame_ms) / 1000; }

void pdm_set_play_volume(int v) {
    if (v < 0) v = 0; if (v > 21) v = 21;
    play_volume = v;
}
int pdm_get_play_volume(void) { return play_volume; }
