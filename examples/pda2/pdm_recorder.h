/**
 * @file      pdm_recorder.h
 * @brief     PDM microphone recorder with streaming Opus encoding to SD,
 *            plus Opus → I2S playback. Designed for press-to-stop UX with a cap.
 *
 * File format (.opus extension, NOT standard Ogg — playable only on-device):
 *   [4B] "OPUS"  magic
 *   [1B] version=1
 *   [1B] channels=1
 *   [2B] uint16_t frame_samples (e.g. 320 for 20ms@16kHz)
 *   [4B] uint32_t sample_rate   (e.g. 16000)
 *   [4B] uint32_t bitrate       (e.g. 24000)
 *   [2B] reserved
 *   ... per-frame: [2B] pkt_len (0=EOF) [pkt_len bytes]
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- legacy fixed-duration API (still used by ui_voice_ai) ---------- */
bool pdm_record_wav(int duration_sec, int sample_rate, uint8_t **wav_out, size_t *wav_len);
void pdm_restore_audio(void);

/* ---------- streaming Opus API ---------- */

/* Start streaming Opus recording to SD at `path`. Spawns a task that auto-stops
 * after `max_seconds` (cap). Returns false if another record is in progress.
 * sample_rate must be 8000/12000/16000/24000/48000.  */
bool pdm_record_start(const char *path, int sample_rate, int max_seconds);

/* Signal current recording to stop. Non-blocking; task closes file and exits. */
void pdm_record_stop(void);

bool pdm_record_is_active(void);
int  pdm_record_elapsed_sec(void);

/* Start streaming Opus playback from SD `path`. Returns false on error.
 * Initialises I2S TX for the file's sample rate, decodes frame-by-frame to I2S.
 * Auto-stops on EOF. */
bool pdm_play_start(const char *path);
void pdm_play_stop(void);
bool pdm_play_is_active(void);
int  pdm_play_elapsed_sec(void);
int  pdm_play_total_sec(void);          /* 0 if unknown */
void pdm_set_play_volume(int v);        /* 0..21, applied to decoded PCM */
int  pdm_get_play_volume(void);

#ifdef __cplusplus
}
#endif
