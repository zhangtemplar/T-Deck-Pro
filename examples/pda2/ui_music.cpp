/**
 * @file      ui_music.cpp
 * @brief     Music player — two screens on the screen stack:
 *              - Now Playing (SCREEN_MUSIC_ID): transport, ID3 info, numeric
 *                progress, volume, and a Browse button.
 *              - Browse (SCREEN_MUSIC_BROWSE_ID): walk /music and sub-folders,
 *                play a single track, "play all in folder", or an .m3u playlist.
 *
 *  Playback uses ESP32-audioI2S (extern Audio audio; pumped by factory.ino's
 *  loop()), so audio keeps running across screen push/pop. Progress is refreshed
 *  at low cadence to limit e-ink ghosting. Synchronized lyrics are deferred.
 */
#include "Arduino.h"
#include "ui_deckpro.h"
#include "ui_deckpro_port.h"
#include "ui_scr_mrg.h"
#include "factory.h"
#include "src/assets.h"
#include <driver/i2s.h>
#include <string.h>
#include <ctype.h>
#include "cjk_font.h"

#define MUSIC_DIR         "/music"
#define MUSIC_MAX_FILES   128
#define BROWSE_MAX        128
#define MUSIC_PAGE_SIZE   5
#define PROGRESS_TICKS    20        /* poll ticks (250ms) between progress updates -> ~5s */

/* Browse entry kinds */
enum { BR_DIR = 0, BR_AUDIO = 1, BR_M3U = 2 };

/* ---- Shared playback state (persists across screen push/pop) ---- */
static char pl_paths[MUSIC_MAX_FILES][96];   /* playback queue, full SD paths */
static int  pl_count = 0;
static int  pl_index = -1;
static uint8_t volume = 15;                  /* 0..21 */
static bool paused = false;
static bool was_running = false;             /* seen isRunning() true since last play */
static bool last_play_ok = false;

/* ID3 metadata (written by audio task via audio_id3data) */
static volatile bool id3_dirty = false;
static char id3_title[64]  = {0};
static char id3_artist[64] = {0};

/* ---- Now Playing widgets ---- */
static lv_obj_t *np_track = NULL;
static lv_obj_t *np_meta  = NULL;
static lv_obj_t *np_time  = NULL;
static lv_obj_t *np_pp_icon = NULL;
static lv_obj_t *np_vol   = NULL;
static lv_timer_t *poll_timer = NULL;
static uint16_t progress_div = 0;

/* ---- Browse state / widgets ---- */
static char browse_dir[96] = MUSIC_DIR;      /* current folder, full SD path */
static char br_name[BROWSE_MAX][64];
static char br_path[BROWSE_MAX][96];
static uint8_t br_type[BROWSE_MAX];
static int  br_count = 0;
static int  br_page = 0;
static lv_obj_t *br_list = NULL;
static lv_obj_t *br_pathlbl = NULL;
static lv_obj_t *br_pagelbl = NULL;
static lv_obj_t *br_up_btn = NULL;
static lv_obj_t *br_playall_btn = NULL;

/* ============================ Helpers ============================ */

static void ensure_audio_init()
{
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = 44100;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 1024;
    i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    i2s_pin_config_t pins = {};
    pins.bck_io_num = BOARD_I2S_BCLK;
    pins.ws_io_num = BOARD_I2S_LRC;
    pins.data_out_num = BOARD_I2S_DOUT;
    pins.data_in_num = I2S_PIN_NO_CHANGE;
    i2s_set_pin(I2S_NUM_0, &pins);
    audio.setPinout(BOARD_I2S_BCLK, BOARD_I2S_LRC, BOARD_I2S_DOUT);
    audio.setVolume(volume);
}

static bool ext_is(const char *name, const char *ext3)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[8] = {0};
    for (int i = 0; i < 6 && dot[i + 1]; i++) ext[i] = tolower((unsigned char)dot[i + 1]);
    return strcmp(ext, ext3) == 0;
}

static bool ext_is_audio(const char *name)
{
    return ext_is(name, "mp3") || ext_is(name, "wav") || ext_is(name, "aac") ||
           ext_is(name, "m4a") || ext_is(name, "ogg") || ext_is(name, "flac");
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

// Turn a possibly-bare dirent name into a full path under `dir`.
static void make_full_path(char *out, size_t n, const char *dir, const char *name)
{
    if (name[0] == '/') { strncpy(out, name, n - 1); out[n - 1] = '\0'; }
    else                  snprintf(out, n, "%s/%s", dir, name);
}

static void fmt_mmss(char *buf, size_t n, uint32_t sec)
{
    snprintf(buf, n, "%lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
}

/* ==================== Now Playing rendering ===================== */

static void update_progress_label()
{
    if (!np_time) return;
    char a[10], b[10];
    uint32_t cur = audio.getAudioCurrentTime();
    uint32_t dur = audio.getAudioFileDuration();
    fmt_mmss(a, sizeof(a), cur);
    if (dur > 0) {
        fmt_mmss(b, sizeof(b), dur);
        lv_label_set_text_fmt(np_time, "%s / %s", a, b);
    } else {
        lv_label_set_text_fmt(np_time, "%s / --:--", a);
    }
}

static void now_playing_refresh()
{
    if (np_track) {
        if (pl_index >= 0 && pl_index < pl_count) {
            const char *base = base_name(pl_paths[pl_index]);
            if (last_play_ok) lv_label_set_text_fmt(np_track, LV_SYMBOL_AUDIO " %s", base);
            else              lv_label_set_text_fmt(np_track, "Play failed: %s", base);
        } else {
            lv_label_set_text(np_track, "No track - open Browse");
        }
    }
    if (np_meta) {
        char buf[140];
        snprintf(buf, sizeof(buf), "%s%s%s",
                 id3_title[0] ? id3_title : "",
                 (id3_title[0] && id3_artist[0]) ? "\n" : "",
                 id3_artist[0] ? id3_artist : "");
        lv_label_set_text(np_meta, buf);
    }
    if (np_pp_icon)
        lv_label_set_text(np_pp_icon, (pl_index >= 0 && !paused && audio.isRunning())
                          ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    update_progress_label();
}

/* ======================== Playback control ===================== */

static void play_index(int idx)
{
    if (idx < 0 || idx >= pl_count) return;
    pl_index = idx;
    paused = false;
    was_running = false;
    id3_title[0] = id3_artist[0] = '\0';

    audio.stopSong();
    ensure_audio_init();
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    last_play_ok = audio.connecttoFS(SD, pl_paths[idx]);
    shared_spi_unlock();

    now_playing_refresh();
}

// Fill the playback queue with every audio file directly under `dir`.
static int fill_queue_from_dir(const char *dir)
{
    pl_count = 0;
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    File d = SD.open(dir);
    if (d && d.isDirectory()) {
        File f = d.openNextFile();
        while (f && pl_count < MUSIC_MAX_FILES) {
            if (!f.isDirectory() && ext_is_audio(f.name())) {
                make_full_path(pl_paths[pl_count], sizeof(pl_paths[0]), dir, f.name());
                pl_count++;
            }
            f = d.openNextFile();
        }
    }
    if (d) d.close();
    shared_spi_unlock();
    return pl_count;
}

// Load an .m3u playlist into the queue. Relative entries resolve against the
// playlist's own directory.
static int load_m3u(const char *path)
{
    pl_count = 0;
    char dir[96];
    strncpy(dir, path, sizeof(dir) - 1); dir[sizeof(dir) - 1] = '\0';
    char *s = strrchr(dir, '/');
    if (s) *s = '\0'; else dir[0] = '\0';

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    File f = SD.open(path);
    if (f) {
        while (f.available() && pl_count < MUSIC_MAX_FILES) {
            String line = f.readStringUntil('\n');
            line.trim();
            if (line.length() == 0 || line[0] == '#') continue;
            if (line[0] == '/') {
                strncpy(pl_paths[pl_count], line.c_str(), sizeof(pl_paths[0]) - 1);
                pl_paths[pl_count][sizeof(pl_paths[0]) - 1] = '\0';
            } else {
                snprintf(pl_paths[pl_count], sizeof(pl_paths[0]), "%s/%s", dir, line.c_str());
            }
            pl_count++;
        }
        f.close();
    }
    shared_spi_unlock();
    return pl_count;
}

/* ===================== Now Playing callbacks ==================== */

static void play_pause_cb(lv_event_t *e)
{
    if (pl_index < 0) { if (pl_count > 0) play_index(0); return; }
    paused = !paused;
    audio.pauseResume();
    if (np_pp_icon) lv_label_set_text(np_pp_icon, paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
}

static void stop_cb(lv_event_t *e)
{
    audio.stopSong();
    paused = false;
    was_running = false;
    if (np_pp_icon) lv_label_set_text(np_pp_icon, LV_SYMBOL_PLAY);
    if (np_time)    lv_label_set_text(np_time, "0:00 / --:--");
}

static void prev_cb(lv_event_t *e)
{
    if (pl_count == 0) return;
    play_index(pl_index <= 0 ? pl_count - 1 : pl_index - 1);
}

static void next_cb(lv_event_t *e)
{
    if (pl_count == 0) return;
    play_index((pl_index + 1) % pl_count);
}

static void vol_up_cb(lv_event_t *e)
{
    if (volume < 21) volume++;
    audio.setVolume(volume);
    if (np_vol) lv_label_set_text_fmt(np_vol, "Vol %d", volume);
}

static void vol_down_cb(lv_event_t *e)
{
    if (volume > 0) volume--;
    audio.setVolume(volume);
    if (np_vol) lv_label_set_text_fmt(np_vol, "Vol %d", volume);
}

static void browse_open_cb(lv_event_t *e)
{
    scr_mgr_push(SCREEN_MUSIC_BROWSE_ID, false);
}

static void music_back_cb(lv_event_t *e)
{
    scr_mgr_pop(false);   /* destroy() stops playback and tears down */
}

static void poll_cb(lv_timer_t *t)
{
    if (id3_dirty) {
        id3_dirty = false;
        if (np_meta) {
            char buf[140];
            snprintf(buf, sizeof(buf), "%s%s%s",
                     id3_title[0] ? id3_title : "",
                     (id3_title[0] && id3_artist[0]) ? "\n" : "",
                     id3_artist[0] ? id3_artist : "");
            lv_label_set_text(np_meta, buf);
        }
    }

    if (audio.isRunning()) was_running = true;

    /* Auto-advance once a track has genuinely finished. */
    if (pl_index >= 0 && !paused && was_running && !audio.isRunning()) {
        was_running = false;
        if (pl_count > 0) play_index((pl_index + 1) % pl_count);
    }

    /* Low-cadence numeric progress to limit e-ink refreshes. */
    if (++progress_div >= PROGRESS_TICKS) {
        progress_div = 0;
        if (pl_index >= 0) update_progress_label();
    }
}

/* ==================== Small UI helper ========================== */

static lv_obj_t *small_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, lv_coord_t w)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, 30);
    lv_obj_set_style_radius(b, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_white(), LV_PART_MAIN);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_black(), LV_PART_MAIN);
    lv_obj_center(l);
    return b;
}

/* ==================== Now Playing screen ======================= */

static void music_create(lv_obj_t *parent)
{
    scr_back_btn_create(parent, "Music", music_back_cb);

    lv_obj_t *browse_btn = small_btn(parent, LV_SYMBOL_DIRECTORY "  Browse Music", browse_open_cb, 200);
    lv_obj_align(browse_btn, LV_ALIGN_TOP_MID, 0, 34);

    np_track = lv_label_create(parent);
    lv_obj_set_width(np_track, 230);
    lv_label_set_long_mode(np_track, LV_LABEL_LONG_DOT);
    lv_obj_align(np_track, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_set_style_text_font(np_track, &g_font_cn, LV_PART_MAIN);

    np_meta = lv_label_create(parent);
    lv_obj_set_width(np_meta, 230);
    lv_label_set_long_mode(np_meta, LV_LABEL_LONG_WRAP);
    lv_obj_align(np_meta, LV_ALIGN_TOP_MID, 0, 104);
    lv_obj_set_style_text_font(np_meta, &g_font_cn, LV_PART_MAIN);
    lv_label_set_text(np_meta, "");

    np_time = lv_label_create(parent);
    lv_obj_align(np_time, LV_ALIGN_TOP_MID, 0, 152);
    lv_obj_set_style_text_font(np_time, &Font_Mono_Bold_14, LV_PART_MAIN);
    lv_label_set_text(np_time, "0:00 / --:--");

    /* Transport row: prev / play-pause / stop / next */
    lv_obj_t *row1 = lv_obj_create(parent);
    lv_obj_set_size(row1, 230, 38);
    lv_obj_align(row1, LV_ALIGN_BOTTOM_MID, 0, -38);
    lv_obj_set_style_pad_all(row1, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(row1, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row1, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(row1, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *prev_btn = small_btn(row1, LV_SYMBOL_PREV, prev_cb, 50);
    lv_obj_align(prev_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *pp_btn = small_btn(row1, LV_SYMBOL_PLAY, play_pause_cb, 60);
    lv_obj_align(pp_btn, LV_ALIGN_LEFT_MID, 56, 0);
    np_pp_icon = lv_obj_get_child(pp_btn, 0);
    lv_obj_t *stop_btn = small_btn(row1, LV_SYMBOL_STOP, stop_cb, 50);
    lv_obj_align(stop_btn, LV_ALIGN_LEFT_MID, 122, 0);
    lv_obj_t *next_btn = small_btn(row1, LV_SYMBOL_NEXT, next_cb, 50);
    lv_obj_align(next_btn, LV_ALIGN_RIGHT_MID, 0, 0);

    /* Volume row */
    lv_obj_t *row2 = lv_obj_create(parent);
    lv_obj_set_size(row2, 230, 34);
    lv_obj_align(row2, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_pad_all(row2, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(row2, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row2, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(row2, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *vd = small_btn(row2, LV_SYMBOL_MINUS, vol_down_cb, 50);
    lv_obj_align(vd, LV_ALIGN_LEFT_MID, 0, 0);
    np_vol = lv_label_create(row2);
    lv_obj_set_style_text_font(np_vol, &Font_Mono_Bold_14, LV_PART_MAIN);
    lv_label_set_text_fmt(np_vol, "Vol %d", volume);
    lv_obj_align(np_vol, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *vu = small_btn(row2, LV_SYMBOL_PLUS, vol_up_cb, 50);
    lv_obj_align(vu, LV_ALIGN_RIGHT_MID, 0, 0);

    /* Default queue = flat /music so Play works before browsing. */
    if (pl_count == 0) fill_queue_from_dir(MUSIC_DIR);
    now_playing_refresh();

    if (!poll_timer) poll_timer = lv_timer_create(poll_cb, 250, NULL);
}

static void music_entry(void)
{
    ui_set_gesture_callback(NULL);   /* no paging on this screen */
    now_playing_refresh();           /* reflect any track change from Browse */
    ui_disp_full_refr();
}

static void music_exit(void)
{
    /* Do NOT stop playback here: exit() also fires when Browse is pushed. */
}

static void music_destroy(void)
{
    /* Real teardown (leaving the app back to the menu). */
    if (poll_timer) { lv_timer_del(poll_timer); poll_timer = NULL; }
    audio.stopSong();
    np_track = np_meta = np_time = np_pp_icon = np_vol = NULL;
    pl_count = 0;
    pl_index = -1;
    paused = false;
    was_running = false;
    strncpy(browse_dir, MUSIC_DIR, sizeof(browse_dir) - 1);
}

/* ======================== Browse screen ======================== */

static int browse_pages()
{
    if (br_count == 0) return 1;
    return (br_count + MUSIC_PAGE_SIZE - 1) / MUSIC_PAGE_SIZE;
}

static void browse_item_cb(lv_event_t *e);

static void browse_render_page()
{
    if (!br_list) return;
    lv_obj_clean(br_list);
    int start = br_page * MUSIC_PAGE_SIZE;
    int end = start + MUSIC_PAGE_SIZE;
    if (end > br_count) end = br_count;
    for (int i = start; i < end; i++) {
        const char *icon = br_type[i] == BR_DIR ? LV_SYMBOL_DIRECTORY :
                           br_type[i] == BR_M3U ? LV_SYMBOL_LIST :
                           LV_SYMBOL_AUDIO;
        lv_obj_t *btn = lv_list_add_btn(br_list, icon, br_name[i]);
        lv_obj_add_event_cb(btn, browse_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (br_pagelbl) lv_label_set_text_fmt(br_pagelbl, "%d/%d", br_page + 1, browse_pages());
}

static void browse_refresh()
{
    br_count = 0;
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    if (!SD.exists(browse_dir)) SD.mkdir(browse_dir);
    File d = SD.open(browse_dir);
    if (d && d.isDirectory()) {
        File f = d.openNextFile();
        while (f && br_count < BROWSE_MAX) {
            const char *base = base_name(f.name());
            if (base[0] != '.') {
                int type = -1;
                if (f.isDirectory())          type = BR_DIR;
                else if (ext_is(base, "m3u")) type = BR_M3U;
                else if (ext_is_audio(base))  type = BR_AUDIO;
                if (type >= 0) {
                    strncpy(br_name[br_count], base, sizeof(br_name[0]) - 1);
                    br_name[br_count][sizeof(br_name[0]) - 1] = '\0';
                    make_full_path(br_path[br_count], sizeof(br_path[0]), browse_dir, f.name());
                    br_type[br_count] = (uint8_t)type;
                    br_count++;
                }
            }
            f = d.openNextFile();
        }
    }
    if (d) d.close();
    shared_spi_unlock();

    if (br_page >= browse_pages()) br_page = browse_pages() - 1;
    if (br_page < 0) br_page = 0;

    if (br_pathlbl) lv_label_set_text(br_pathlbl, browse_dir);

    bool at_root = (strcmp(browse_dir, MUSIC_DIR) == 0);
    bool has_audio = false;
    for (int i = 0; i < br_count; i++) if (br_type[i] == BR_AUDIO) { has_audio = true; break; }
    if (br_up_btn)      at_root  ? lv_obj_add_flag(br_up_btn, LV_OBJ_FLAG_HIDDEN)
                                 : lv_obj_clear_flag(br_up_btn, LV_OBJ_FLAG_HIDDEN);
    if (br_playall_btn) has_audio ? lv_obj_clear_flag(br_playall_btn, LV_OBJ_FLAG_HIDDEN)
                                  : lv_obj_add_flag(br_playall_btn, LV_OBJ_FLAG_HIDDEN);

    browse_render_page();
}

static void browse_item_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= br_count) return;

    if (br_type[i] == BR_DIR) {
        strncpy(browse_dir, br_path[i], sizeof(browse_dir) - 1);
        browse_dir[sizeof(browse_dir) - 1] = '\0';
        br_page = 0;
        browse_refresh();
    } else if (br_type[i] == BR_M3U) {
        if (load_m3u(br_path[i]) > 0) {
            play_index(0);
            scr_mgr_pop(false);
        }
    } else { /* BR_AUDIO: queue the whole folder, start at this file */
        char chosen[96];
        strncpy(chosen, br_path[i], sizeof(chosen) - 1);
        chosen[sizeof(chosen) - 1] = '\0';
        fill_queue_from_dir(browse_dir);
        int sel = 0;
        for (int k = 0; k < pl_count; k++) if (strcmp(pl_paths[k], chosen) == 0) { sel = k; break; }
        play_index(sel);
        scr_mgr_pop(false);
    }
}

static void browse_up_cb(lv_event_t *e)
{
    if (strcmp(browse_dir, MUSIC_DIR) == 0) return;
    char *s = strrchr(browse_dir, '/');
    if (s && s != browse_dir) *s = '\0';
    if (strlen(browse_dir) < strlen(MUSIC_DIR))
        strncpy(browse_dir, MUSIC_DIR, sizeof(browse_dir) - 1);
    br_page = 0;
    browse_refresh();
}

static void browse_playall_cb(lv_event_t *e)
{
    if (fill_queue_from_dir(browse_dir) > 0) {
        play_index(0);
        scr_mgr_pop(false);
    }
}

static void browse_back_cb(lv_event_t *e)
{
    scr_mgr_pop(false);   /* return to Now Playing; playback continues */
}

static void browse_gesture_cb(int dir)
{
    int n = browse_pages();
    if (n <= 1) return;
    if (dir == LV_DIR_LEFT && br_page < n - 1)      { br_page++; browse_render_page(); }
    else if (dir == LV_DIR_RIGHT && br_page > 0)    { br_page--; browse_render_page(); }
}

static void browse_create(lv_obj_t *parent)
{
    scr_back_btn_create(parent, "Browse", browse_back_cb);

    br_pagelbl = lv_label_create(parent);
    lv_obj_align(br_pagelbl, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_set_style_text_font(br_pagelbl, &Font_Mono_Bold_14, LV_PART_MAIN);
    lv_label_set_text(br_pagelbl, "1/1");

    br_pathlbl = lv_label_create(parent);
    lv_obj_set_width(br_pathlbl, 230);
    lv_label_set_long_mode(br_pathlbl, LV_LABEL_LONG_DOT);
    lv_obj_align(br_pathlbl, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_text_font(br_pathlbl, &Font_Mono_Bold_14, LV_PART_MAIN);
    lv_label_set_text(br_pathlbl, browse_dir);

    br_up_btn = small_btn(parent, LV_SYMBOL_UP "  Up", browse_up_cb, 95);
    lv_obj_align(br_up_btn, LV_ALIGN_TOP_LEFT, 8, 56);
    br_playall_btn = small_btn(parent, LV_SYMBOL_PLAY "  All", browse_playall_cb, 95);
    lv_obj_align(br_playall_btn, LV_ALIGN_TOP_RIGHT, -8, 56);

    br_list = lv_list_create(parent);
    lv_obj_set_size(br_list, 230, 210);
    lv_obj_align(br_list, LV_ALIGN_TOP_MID, 0, 92);
    lv_obj_set_style_pad_all(br_list, 2, LV_PART_MAIN);
    /* CJK-capable font so Chinese file/folder names render (coverage limited
     * to the font's glyph set until regenerated). */
    lv_obj_set_style_text_font(br_list, &g_font_cn, LV_PART_MAIN);

    browse_refresh();
}

static void browse_entry(void)
{
    ui_set_gesture_callback(browse_gesture_cb);
    ui_disp_full_refr();
}

static void browse_exit(void)
{
    ui_set_gesture_callback(NULL);
}

static void browse_destroy(void)
{
    br_list = br_pathlbl = br_pagelbl = br_up_btn = br_playall_btn = NULL;
    br_count = 0;
    br_page = 0;
}

/* ============ Audio library ID3 callback (audio task) ============ */

void audio_id3data(const char *info)
{
    if (!info) return;
    if (strncmp(info, "Title: ", 7) == 0) {
        strncpy(id3_title, info + 7, sizeof(id3_title) - 1);
        id3_title[sizeof(id3_title) - 1] = '\0';
        id3_dirty = true;
    } else if (strncmp(info, "Artist: ", 8) == 0) {
        strncpy(id3_artist, info + 8, sizeof(id3_artist) - 1);
        id3_artist[sizeof(id3_artist) - 1] = '\0';
        id3_dirty = true;
    }
}

extern "C" {
scr_lifecycle_t screen_music = {
    .create  = music_create,
    .entry   = music_entry,
    .exit    = music_exit,
    .destroy = music_destroy,
};

scr_lifecycle_t screen_music_browse = {
    .create  = browse_create,
    .entry   = browse_entry,
    .exit    = browse_exit,
    .destroy = browse_destroy,
};
}
