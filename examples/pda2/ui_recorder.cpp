/**
 * @file      ui_recorder.cpp
 * @brief     Voice recorder — streaming Opus to SD with press-to-stop and 30-min cap.
 *            File list paginated; swipe left/right to change page.
 */
#include "Arduino.h"
#include "ui_deckpro.h"
#include "ui_deckpro_port.h"
#include "factory.h"
#include "pdm_recorder.h"
#include "src/assets.h"
#include "peripheral.h"
#include <time.h>

#define REC_DIR          "/recordings"
#define REC_MAX_FILES    128
#define REC_PAGE_SIZE    7
#define REC_MAX_SECONDS  (30 * 60)
#define REC_SAMPLE_RATE  16000

static lv_obj_t *rec_btn = NULL;
static lv_obj_t *rec_btn_label = NULL;
static lv_obj_t *file_list = NULL;
static lv_obj_t *status_label = NULL;
static lv_obj_t *progress_label = NULL;
static lv_obj_t *vol_label = NULL;
static lv_obj_t *page_label = NULL;
static lv_obj_t *action_bar = NULL;

static char file_paths[REC_MAX_FILES][80];
static int file_count = 0;
static int selected_idx = -1;
static int cur_page = 0;
static bool rec_screen_active = false;

static lv_timer_t *poll_timer = NULL;

static int total_pages()
{
    if (file_count == 0) return 1;
    return (file_count + REC_PAGE_SIZE - 1) / REC_PAGE_SIZE;
}

static void file_item_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= file_count) return;
    selected_idx = (int)idx;
    if (action_bar) lv_obj_clear_flag(action_bar, LV_OBJ_FLAG_HIDDEN);

    /* Stop any prior playback, start new */
    pdm_play_stop();
    /* small wait — playback task tears down i2s; pdm_play_start will reinstall */
    delay(20);
    bool ok = pdm_play_start(file_paths[idx]);
    const char *base = strrchr(file_paths[idx], '/');
    base = base ? base + 1 : file_paths[idx];
    if (status_label) {
        if (ok) lv_label_set_text_fmt(status_label, "Playing: %s", base);
        else    lv_label_set_text_fmt(status_label, "Play failed: %s", base);
    }
}

static void render_page()
{
    if (!file_list) return;
    lv_obj_clean(file_list);
    int start = cur_page * REC_PAGE_SIZE;
    int end = start + REC_PAGE_SIZE;
    if (end > file_count) end = file_count;
    for (int i = start; i < end; i++) {
        const char *base = strrchr(file_paths[i], '/');
        base = base ? base + 1 : file_paths[i];
        lv_obj_t *btn = lv_list_add_btn(file_list, LV_SYMBOL_AUDIO, base);
        lv_obj_add_event_cb(btn, file_item_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }
    if (page_label) lv_label_set_text_fmt(page_label, "%d/%d", cur_page + 1, total_pages());
}

static void refresh_file_list()
{
    file_count = 0;
    selected_idx = -1;
    if (action_bar) lv_obj_add_flag(action_bar, LV_OBJ_FLAG_HIDDEN);

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    if (!SD.exists(REC_DIR)) SD.mkdir(REC_DIR);
    File dir = SD.open(REC_DIR);
    if (!dir || !dir.isDirectory()) {
        shared_spi_unlock();
        if (status_label) lv_label_set_text(status_label, "SD not available");
        return;
    }
    File f = dir.openNextFile();
    while (f && file_count < REC_MAX_FILES) {
        if (!f.isDirectory()) {
            const char *name = f.name();
            if (name[0] != '/') {
                snprintf(file_paths[file_count], sizeof(file_paths[0]), REC_DIR "/%s", name);
            } else {
                strncpy(file_paths[file_count], name, sizeof(file_paths[0]) - 1);
            }
            file_count++;
        }
        f = dir.openNextFile();
    }
    dir.close();
    shared_spi_unlock();

    if (cur_page >= total_pages()) cur_page = total_pages() - 1;
    if (cur_page < 0) cur_page = 0;
    render_page();
    if (status_label) lv_label_set_text_fmt(status_label, "%d recording(s)", file_count);
}

static void rec_btn_cb(lv_event_t *e)
{
    if (pdm_record_is_active()) {
        pdm_record_stop();
        if (status_label) lv_label_set_text(status_label, "Stopping...");
        return;
    }
    pdm_play_stop();
    delay(20);

    char path[80];
    time_t now;
    time(&now);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(path, sizeof(path), REC_DIR "/rec_%04d%02d%02d_%02d%02d%02d.opus",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    if (!pdm_record_start(path, REC_SAMPLE_RATE, REC_MAX_SECONDS)) {
        if (status_label) lv_label_set_text(status_label, "Start failed");
        return;
    }
    if (rec_btn_label) lv_label_set_text(rec_btn_label, LV_SYMBOL_STOP "  Stop");
    if (status_label) lv_label_set_text(status_label, "Recording 0:00 (cap 30:00)");
}

static void delete_btn_cb(lv_event_t *e)
{
    if (selected_idx < 0 || selected_idx >= file_count) return;
    pdm_play_stop();
    delay(20);
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    SD.remove(file_paths[selected_idx]);
    shared_spi_unlock();
    refresh_file_list();
}

static void stop_btn_cb(lv_event_t *e)
{
    pdm_play_stop();
    if (status_label) lv_label_set_text(status_label, "Stopped");
}

static void poll_cb(lv_timer_t *t)
{
    static bool was_recording = false;
    static bool was_playing = false;
    bool recording = pdm_record_is_active();
    bool playing   = pdm_play_is_active();

    if (recording) {
        static int last_sec = -1;
        int sec = pdm_record_elapsed_sec();
        if (sec != last_sec) {
            last_sec = sec;
            if (status_label)
                lv_label_set_text_fmt(status_label, "REC %d:%02d (max %d:00)",
                                      sec / 60, sec % 60, REC_MAX_SECONDS / 60);
            if (progress_label) lv_label_set_text(progress_label, "");
        }
    } else if (was_recording) {
        if (rec_btn_label) lv_label_set_text(rec_btn_label, LV_SYMBOL_PLAY "  Record");
        if (status_label)  lv_label_set_text(status_label, "Saved");
        refresh_file_list();
    } else if (playing) {
        int e = pdm_play_elapsed_sec();
        int t = pdm_play_total_sec();
        if (progress_label) {
            if (t > 0)
                lv_label_set_text_fmt(progress_label, "%d:%02d / %d:%02d",
                                      e / 60, e % 60, t / 60, t % 60);
            else
                lv_label_set_text_fmt(progress_label, "%d:%02d", e / 60, e % 60);
        }
    } else if (was_playing) {
        if (progress_label) lv_label_set_text(progress_label, "");
        if (status_label)   lv_label_set_text(status_label, "Done");
    }

    was_recording = recording;
    was_playing   = playing;
}

/* Keyboard: + / - from Sym layer = volume up/down */
void recorder_keyboard_poll(void)
{
    if (!rec_screen_active) return;
    char c;
    if (!keypad_get_val(&c)) return;
    keypad_set_flag();
    int v = pdm_get_play_volume();
    if (c == '+' && v < 21) v++;
    else if (c == '-' && v > 0) v--;
    else return;
    pdm_set_play_volume(v);
    if (vol_label) lv_label_set_text_fmt(vol_label, "Vol %d", v);
}

static void gesture_cb(int dir)
{
    int n = total_pages();
    if (n <= 1) return;
    if (dir == LV_DIR_LEFT && cur_page < n - 1)      { cur_page++; render_page(); }
    else if (dir == LV_DIR_RIGHT && cur_page > 0)    { cur_page--; render_page(); }
}

static void rec_back_cb(lv_event_t *e)
{
    pdm_record_stop();
    pdm_play_stop();
    scr_mgr_pop(false);
}

static void rec_create(lv_obj_t *parent)
{
    scr_back_btn_create(parent, "Recorder", rec_back_cb);

    /* REC button — wide top control */
    rec_btn = lv_btn_create(parent);
    lv_obj_set_size(rec_btn, 230, 40);
    lv_obj_align(rec_btn, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_radius(rec_btn, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(rec_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rec_btn, lv_color_white(), LV_PART_MAIN);
    lv_obj_add_event_cb(rec_btn, rec_btn_cb, LV_EVENT_CLICKED, NULL);
    rec_btn_label = lv_label_create(rec_btn);
    lv_label_set_text(rec_btn_label, LV_SYMBOL_PLAY "  Record");
    lv_obj_set_style_text_color(rec_btn_label, lv_color_black(), LV_PART_MAIN);
    lv_obj_center(rec_btn_label);

    /* File list */
    file_list = lv_list_create(parent);
    lv_obj_set_size(file_list, 230, 160);
    lv_obj_align(file_list, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_set_style_pad_all(file_list, 2, LV_PART_MAIN);

    /* Action bar (Stop + Delete) hidden until selection */
    action_bar = lv_obj_create(parent);
    lv_obj_set_size(action_bar, 230, 30);
    lv_obj_align(action_bar, LV_ALIGN_BOTTOM_MID, 0, -22);
    /* file_list bottom = 78+160 = 238; action_bar top = 320-22-30 = 268; progress at -54 fits in 238..268 */
    lv_obj_set_style_pad_all(action_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(action_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(action_bar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(action_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(action_bar, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *stop_btn = lv_btn_create(action_bar);
    lv_obj_set_size(stop_btn, 70, 26);
    lv_obj_align(stop_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(stop_btn, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(stop_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(stop_btn, lv_color_white(), LV_PART_MAIN);
    lv_obj_add_event_cb(stop_btn, stop_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(stop_btn);
    lv_label_set_text(sl, LV_SYMBOL_STOP " Stop");
    lv_obj_set_style_text_color(sl, lv_color_black(), LV_PART_MAIN);
    lv_obj_center(sl);

    lv_obj_t *del_btn = lv_btn_create(action_bar);
    lv_obj_set_size(del_btn, 80, 26);
    lv_obj_align(del_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(del_btn, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(del_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(del_btn, lv_color_white(), LV_PART_MAIN);
    lv_obj_add_event_cb(del_btn, delete_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(del_btn);
    lv_label_set_text(dl, LV_SYMBOL_TRASH " Delete");
    lv_obj_set_style_text_color(dl, lv_color_black(), LV_PART_MAIN);
    lv_obj_center(dl);

    /* Progress + volume row (above action bar) */
    progress_label = lv_label_create(parent);
    lv_obj_align(progress_label, LV_ALIGN_BOTTOM_LEFT, 4, -54);
    lv_obj_set_style_text_font(progress_label, &Font_Mono_Bold_14, LV_PART_MAIN);
    lv_label_set_text(progress_label, "");

    vol_label = lv_label_create(parent);
    lv_obj_align(vol_label, LV_ALIGN_BOTTOM_RIGHT, -4, -54);
    lv_obj_set_style_text_font(vol_label, &Font_Mono_Bold_14, LV_PART_MAIN);
    lv_label_set_text_fmt(vol_label, "Vol %d (Sym+o/i)", pdm_get_play_volume());

    /* Bottom footer: status + page indicator */
    status_label = lv_label_create(parent);
    lv_obj_set_width(status_label, 180);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_LEFT, 4, -2);
    lv_obj_set_style_text_font(status_label, &Font_Mono_Bold_14, LV_PART_MAIN);
    lv_label_set_text(status_label, "Ready");

    page_label = lv_label_create(parent);
    lv_obj_align(page_label, LV_ALIGN_BOTTOM_RIGHT, -4, -2);
    lv_obj_set_style_text_font(page_label, &Font_Mono_Bold_14, LV_PART_MAIN);
    lv_label_set_text(page_label, "1/1");

    refresh_file_list();
    if (!poll_timer) poll_timer = lv_timer_create(poll_cb, 500, NULL);
}

static void rec_entry(void)
{
    rec_screen_active = true;
    ui_set_gesture_callback(gesture_cb);
    ui_disp_full_refr();
}
static void rec_exit(void)
{
    rec_screen_active = false;
    ui_set_gesture_callback(NULL);
    pdm_record_stop();
    pdm_play_stop();
    ui_disp_full_refr();
}
static void rec_destroy(void)
{
    if (poll_timer) { lv_timer_del(poll_timer); poll_timer = NULL; }
    rec_btn = rec_btn_label = file_list = status_label = page_label = action_bar = NULL;
    progress_label = vol_label = NULL;
    file_count = 0;
    selected_idx = -1;
    cur_page = 0;
}

extern "C" {
scr_lifecycle_t screen_recorder = {
    .create  = rec_create,
    .entry   = rec_entry,
    .exit    = rec_exit,
    .destroy = rec_destroy,
};
}
