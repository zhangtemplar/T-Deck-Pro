/**
 * @file      ui_voice_ai.cpp
 * @brief     Voice AI app using Google Gemini API (text mode).
 *            Chat interface: type question, get AI response.
 *            Uses pagination for long responses.
 */
#include "Arduino.h"
#include "ui_deckpro.h"
#include "ui_deckpro_port.h"
#include "power_mgr.h"
#include "gemini_api.h"
#include "pdm_recorder.h"
#include "config_keys.h"
#include "app_config.h"
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/queue.h>
#include "Audio.h"
#include "utilities.h"
#include <driver/i2s.h>

static lv_obj_t *response_label = NULL;
static lv_obj_t *input_ta = NULL;
static lv_obj_t *status_label = NULL;
static TaskHandle_t ai_task = NULL;
static bool ai_kbd_active = false;
static bool tts_playing = false;
static volatile bool tts_auto_read = false;

static char *chat_history = NULL;
static char *last_response = NULL;

extern Audio audio;

/* Thread-safe UI queue */
enum { UI_MSG_APPEND = 1, UI_MSG_STATUS = 2 };
struct ui_msg_t { int type; char *text; };
static QueueHandle_t ui_queue = NULL;
static lv_timer_t *ui_timer = NULL;

/* Pagination for response display */
#define RESPONSE_PAGE_CHARS 400
static int response_page = 0;
static int response_total_pages = 1;

static void chat_append(const char *text)
{
    if (!text || !text[0]) return;
    if (chat_history) {
        size_t old_len = strlen(chat_history);
        size_t new_len = strlen(text);
        char *buf = (char *)heap_caps_realloc(chat_history, old_len + new_len + 3,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = (char *)realloc(chat_history, old_len + new_len + 3);
        if (buf) {
            buf[old_len] = '\n';
            memcpy(buf + old_len + 1, text, new_len + 1);
            chat_history = buf;
        }
    } else {
        size_t len = strlen(text);
        chat_history = (char *)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (chat_history) memcpy(chat_history, text, len + 1);
        else chat_history = strdup(text);
    }
    /* Show last page */
    if (chat_history) {
        size_t total = strlen(chat_history);
        response_total_pages = (total + RESPONSE_PAGE_CHARS - 1) / RESPONSE_PAGE_CHARS;
        if (response_total_pages < 1) response_total_pages = 1;
        response_page = response_total_pages - 1;

        /* Display current page */
        int start = response_page * RESPONSE_PAGE_CHARS;
        int len = total - start;
        if (len > RESPONSE_PAGE_CHARS) len = RESPONSE_PAGE_CHARS;
        static char page_buf[RESPONSE_PAGE_CHARS + 1];
        memcpy(page_buf, chat_history + start, len);
        page_buf[len] = '\0';
        lv_label_set_text(response_label, page_buf);
    }
}

static void show_response_page(int pg)
{
    if (!chat_history) return;
    size_t total = strlen(chat_history);
    response_total_pages = (total + RESPONSE_PAGE_CHARS - 1) / RESPONSE_PAGE_CHARS;
    if (pg < 0) pg = 0;
    if (pg >= response_total_pages) pg = response_total_pages - 1;
    response_page = pg;

    int start = pg * RESPONSE_PAGE_CHARS;
    int len = total - start;
    if (len > RESPONSE_PAGE_CHARS) len = RESPONSE_PAGE_CHARS;
    static char page_buf[RESPONSE_PAGE_CHARS + 1];
    memcpy(page_buf, chat_history + start, len);
    page_buf[len] = '\0';
    lv_label_set_text(response_label, page_buf);
    if (status_label)
        lv_label_set_text_fmt(status_label, "Page %d/%d  W:up S:down", pg + 1, response_total_pages);
}

static void chat_show_status(const char *s)
{
    if (status_label) lv_label_set_text(status_label, s);
}

static void ui_post(int type, const char *text)
{
    if (!ui_queue) return;
    ui_msg_t msg;
    msg.type = type;
    msg.text = strdup(text);
    if (xQueueSend(ui_queue, &msg, pdMS_TO_TICKS(2000)) != pdTRUE)
        free(msg.text);
}

static void start_tts();

static void ui_timer_cb(lv_timer_t *t)
{
    ui_msg_t msg;
    while (xQueueReceive(ui_queue, &msg, 0) == pdTRUE) {
        if (msg.type == UI_MSG_APPEND) chat_append(msg.text);
        else if (msg.type == UI_MSG_STATUS) chat_show_status(msg.text);
        free(msg.text);
    }

    if (tts_auto_read && ai_task == NULL) {
        tts_auto_read = false;
        start_tts();
    }

    if (tts_playing && !audio.isRunning()) {
        tts_playing = false;
        if (status_label) lv_label_set_text(status_label, "V:voice R:read Enter:text");
    }
}

static void ai_text_task(void *param)
{
    char *prompt = (char *)param;
    if (!cfg_has(CFG_GEMINI_KEY)) {
        ui_post(UI_MSG_APPEND, "Set gemini_api_key in /config_keys.ini");
        free(prompt);
        ai_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    Serial.printf("[VoiceAI] prompt: %s\n", prompt);
    ui_post(UI_MSG_STATUS, "Waiting for Gemini...");

    gemini_response_t resp = gemini_send_text(prompt, cfg_get(CFG_GEMINI_KEY));
    if (resp.success) {
        if (last_response) free(last_response);
        last_response = strdup(resp.text.c_str());
        ui_post(UI_MSG_APPEND, resp.text.c_str());
        ui_post(UI_MSG_STATUS, "V:voice R:read Enter:text");
    } else {
        char buf[256];
        snprintf(buf, sizeof(buf), "Error: %s", resp.error.c_str());
        ui_post(UI_MSG_APPEND, buf);
        ui_post(UI_MSG_STATUS, "V:voice Enter:text");
    }
    free(prompt);
    ai_task = NULL;
    vTaskDelete(NULL);
}

static void ai_voice_task(void *param)
{
    if (!cfg_has(CFG_GEMINI_KEY)) {
        ui_post(UI_MSG_APPEND, "Set gemini_api_key in /config_keys.ini");
        ai_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ui_post(UI_MSG_STATUS, "Recording 5 sec...");
    ui_post(UI_MSG_APPEND, "> [Voice recording]");

    uint8_t *wav = NULL;
    size_t wav_len = 0;
    bool ok = pdm_record_wav(5, 16000, &wav, &wav_len);
    pdm_restore_audio();

    if (ok && wav && wav_len > 0) {
        ui_post(UI_MSG_STATUS, "Sending to Gemini...");
        gemini_response_t resp = gemini_send_audio(wav, wav_len, cfg_get(CFG_GEMINI_KEY));
        free(wav);

        if (resp.success) {
            if (last_response) free(last_response);
            last_response = strdup(resp.text.c_str());
            ui_post(UI_MSG_APPEND, resp.text.c_str());
            tts_auto_read = true;
        } else {
            char buf[256];
            snprintf(buf, sizeof(buf), "Error: %s", resp.error.c_str());
            ui_post(UI_MSG_APPEND, buf);
            ui_post(UI_MSG_STATUS, "V:voice Enter:text");
        }
    } else {
        if (wav) free(wav);
        ui_post(UI_MSG_APPEND, "Recording failed");
        ui_post(UI_MSG_STATUS, "V:voice Enter:text");
    }
    ai_task = NULL;
    vTaskDelete(NULL);
}

static void start_voice_record()
{
    if (ai_task) return;
    if (WiFi.status() != WL_CONNECTED) {
        chat_append("WiFi not connected.");
        return;
    }
    xTaskCreatePinnedToCore(ai_voice_task, "ai_voice", 16384, NULL, 5, &ai_task, 0);
}

static void ensure_audio_init()
{
    Serial.println("[VoiceAI] Re-initializing audio...");

    /* The Audio library's internal I2S handle is stale after PDM recording.
     * We must install a basic I2S TX driver so setPinout has a valid handle.
     * The Audio library will reconfigure it as needed for playback. */
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = 44100;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 1024;

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    Serial.printf("[VoiceAI] i2s_driver_install: %s\n", esp_err_to_name(err));

    i2s_pin_config_t pins = {};
    pins.bck_io_num = BOARD_I2S_BCLK;
    pins.ws_io_num = BOARD_I2S_LRC;
    pins.data_out_num = BOARD_I2S_DOUT;
    pins.data_in_num = I2S_PIN_NO_CHANGE;
    err = i2s_set_pin(I2S_NUM_0, &pins);
    Serial.printf("[VoiceAI] i2s_set_pin: %s\n", esp_err_to_name(err));

    /* Now Audio.setPinout is safe — driver is installed */
    audio.setPinout(BOARD_I2S_BCLK, BOARD_I2S_LRC, BOARD_I2S_DOUT);
    audio.setVolume(21);
    Serial.println("[VoiceAI] Audio re-initialized");
}

static void start_tts()
{
    Serial.println("[VoiceAI] TTS: start_tts called");

    if (!last_response || last_response[0] == '\0') {
        Serial.println("[VoiceAI] TTS: no response to read");
        if (status_label) lv_label_set_text(status_label, "No response to read");
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[VoiceAI] TTS: no WiFi");
        if (status_label) lv_label_set_text(status_label, "WiFi needed for TTS");
        return;
    }

    Serial.println("[VoiceAI] TTS: ensuring audio init...");
    ensure_audio_init();
    Serial.println("[VoiceAI] TTS: audio init done");

    /* Truncate to ~200 chars for TTS to reduce memory pressure */
    char tts_buf[201];
    strncpy(tts_buf, last_response, 200);
    tts_buf[200] = '\0';

    /* Strip any special chars that might cause issues */
    for (int i = 0; tts_buf[i]; i++) {
        if (tts_buf[i] == '\n' || tts_buf[i] == '\r') tts_buf[i] = ' ';
    }

    Serial.printf("[VoiceAI] TTS: speaking %d chars: \"%.50s...\"\n", (int)strlen(tts_buf), tts_buf);
    if (status_label) lv_label_set_text(status_label, "Reading aloud...");

    Serial.printf("[VoiceAI] TTS: free heap=%d, PSRAM=%d\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());

    bool ok = audio.connecttospeech(tts_buf, "en");
    Serial.printf("[VoiceAI] TTS: connecttospeech returned %d\n", ok);
    tts_playing = ok;
}

static void do_send()
{
    if (!input_ta || ai_task) return;
    const char *text = lv_textarea_get_text(input_ta);
    if (!text || text[0] == '\0') return;

    if (WiFi.status() != WL_CONNECTED) {
        chat_append("WiFi not connected.");
        return;
    }

    /* Show user message */
    char q[270];
    snprintf(q, sizeof(q), "> %s", text);
    chat_append(q);

    char *prompt = strdup(text);
    lv_textarea_set_text(input_ta, "");
    if (prompt)
        xTaskCreatePinnedToCore(ai_text_task, "ai_text", 16384, prompt, 5, &ai_task, 0);
}

/* Keyboard */
void voiceai_keyboard_poll()
{
    if (!ai_kbd_active || !input_ta) return;
    char c;
    if (!keypad_get_val(&c)) return;
    keypad_set_flag();

    if (c == '\n') {
        do_send();
    } else if (c == 'r' && ai_task == NULL) {
        const char *text = lv_textarea_get_text(input_ta);
        if (!text || text[0] == '\0') {
            start_tts();
            return;
        }
        lv_textarea_add_char(input_ta, c);
        return;
    } else if (c == 'v' && ai_task == NULL) {
        const char *text = lv_textarea_get_text(input_ta);
        if (!text || text[0] == '\0') {
            start_voice_record();
            return;
        }
        lv_textarea_add_char(input_ta, c);
        return;
    } else if (c == '\b') {
        const char *text = lv_textarea_get_text(input_ta);
        if (!text || text[0] == '\0') {
            ai_kbd_active = false;
            scr_mgr_pop(false);
        } else {
            lv_textarea_del_char(input_ta);
        }
    } else if (c >= ' ' && c <= '~') {
        lv_textarea_add_char(input_ta, c);
    }
}

static void ai_back_cb(lv_event_t *e)
{
    ai_kbd_active = false;
    scr_mgr_pop(false);
}

static void ai_create(lv_obj_t *parent)
{
    scr_back_btn_create(parent, "Voice AI", ai_back_cb);

    ui_queue = xQueueCreate(8, sizeof(ui_msg_t));
    ui_timer = lv_timer_create(ui_timer_cb, 200, NULL);

    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, 230, 270);
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cont, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cont, 2, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    /* Response area (top, most space) */
    response_label = lv_label_create(cont);
    lv_obj_set_width(response_label, lv_pct(100));
    lv_obj_set_flex_grow(response_label, 1);
    lv_obj_set_style_text_font(response_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_long_mode(response_label, LV_LABEL_LONG_WRAP);

    lv_label_set_text(response_label,
                      cfg_has(CFG_GEMINI_KEY)
                          ? "Enter: send text\nV: voice (5s record)\nR: read last response"
                          : "Set gemini_api_key\nin /config_keys.ini");

    /* Status line */
    status_label = lv_label_create(cont);
    lv_obj_set_width(status_label, lv_pct(100));
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_palette_main(LV_PALETTE_GREY), LV_PART_MAIN);
    lv_label_set_text(status_label, "");

    /* Input at bottom */
    input_ta = lv_textarea_create(cont);
    /* No cursor blink: each blink is a full e-ink refresh, so a focused
     * field would repaint the panel twice a second forever. */
    lv_obj_set_style_anim_time(input_ta, 0, LV_PART_CURSOR);
    lv_obj_set_width(input_ta, lv_pct(100));
    lv_obj_set_height(input_ta, 36);
    lv_textarea_set_placeholder_text(input_ta, "Ask anything...");
    lv_textarea_set_one_line(input_ta, true);
    lv_textarea_set_max_length(input_ta, 256);
    lv_obj_set_style_text_font(input_ta, &lv_font_montserrat_14, LV_PART_MAIN);

    response_page = 0;
    response_total_pages = 1;
    ai_kbd_active = true;
}

static void ai_entry(void) { power_acquire(PWR_WIFI); ui_disp_full_refr(); }
static void ai_exit(void) { power_release(PWR_WIFI); ui_disp_full_refr(); }
static void ai_destroy(void)
{
    ai_kbd_active = false;
    if (ai_task) { vTaskDelete(ai_task); ai_task = NULL; }
    if (ui_timer) { lv_timer_del(ui_timer); ui_timer = NULL; }
    if (ui_queue) {
        ui_msg_t msg;
        while (xQueueReceive(ui_queue, &msg, 0) == pdTRUE) free(msg.text);
        vQueueDelete(ui_queue); ui_queue = NULL;
    }
    if (chat_history) { free(chat_history); chat_history = NULL; }
    if (last_response) { free(last_response); last_response = NULL; }
    tts_playing = false;
    tts_auto_read = false;
    response_label = input_ta = status_label = NULL;
}

scr_lifecycle_t screen_voice_ai = {
    .create = ai_create,
    .entry = ai_entry,
    .exit = ai_exit,
    .destroy = ai_destroy,
};
