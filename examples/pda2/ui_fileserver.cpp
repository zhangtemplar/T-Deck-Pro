/**
 * @file      ui_fileserver.cpp
 * @brief     File Server app — exposes the SD card over HTTP on the local WiFi.
 *
 * On entry it starts the HTTP server (if WiFi is up) and shows the URL to open
 * from a browser on the same network. The server is torn down on exit so the SD
 * bus is only shared while this screen is active.
 */
#include "Arduino.h"
#include "ui_deckpro.h"
#include "ui_deckpro_port.h"
#include "factory.h"
#include "file_server.h"
#include "power_mgr.h"
#include "src/assets.h"
#include <WiFi.h>

static lv_obj_t *fs_state_label = NULL;
static lv_obj_t *fs_url_label = NULL;
static lv_obj_t *fs_hint_label = NULL;
static lv_obj_t *fs_toggle_btn = NULL;
static lv_obj_t *fs_toggle_label = NULL;
static lv_timer_t *fs_timer = NULL;

static void fs_update(void)
{
    bool wifi = (WiFi.status() == WL_CONNECTED);
    bool running = file_server_is_running();

    if (fs_state_label) {
        if (!wifi)
            lv_label_set_text(fs_state_label, "WiFi: not connected");
        else
            lv_label_set_text_fmt(fs_state_label, "WiFi: %s", WiFi.SSID().c_str());
    }

    if (fs_url_label) {
        if (running && file_server_url()[0])
            lv_label_set_text_fmt(fs_url_label, "%s", file_server_url());
        else
            lv_label_set_text(fs_url_label, wifi ? "(stopped)" : "");
    }

    if (fs_toggle_label)
        lv_label_set_text(fs_toggle_label, running ? "Stop server" : "Start server");
}

static void fs_toggle_cb(lv_event_t *e)
{
    if (file_server_is_running()) {
        file_server_stop();
    } else {
        if (!file_server_start()) {
            if (fs_state_label)
                lv_label_set_text(fs_state_label, "WiFi: connecting, retry...");
        }
    }
    fs_update();
    ui_disp_full_refr();
}

static void fs_poll_cb(lv_timer_t *t)
{
    /* Auto-start once WiFi comes up; refresh IP if it changed. */
    static bool last_running = false;
    if (!file_server_is_running() && WiFi.status() == WL_CONNECTED)
        file_server_start();
    bool running = file_server_is_running();
    fs_update();
    if (running != last_running) {
        last_running = running;
        ui_disp_full_refr();
    }
}

static void fs_back_cb(lv_event_t *e)
{
    scr_mgr_pop(false);
}

static void fs_create(lv_obj_t *parent)
{
    scr_back_btn_create(parent, "File Server", fs_back_cb);

    fs_state_label = lv_label_create(parent);
    lv_obj_set_width(fs_state_label, 224);
    lv_label_set_long_mode(fs_state_label, LV_LABEL_LONG_DOT);
    lv_obj_align(fs_state_label, LV_ALIGN_TOP_LEFT, 8, 44);
    lv_obj_set_style_text_font(fs_state_label, &Font_Mono_Bold_14, LV_PART_MAIN);
    lv_label_set_text(fs_state_label, "WiFi: ...");

    lv_obj_t *open_hdr = lv_label_create(parent);
    lv_obj_align(open_hdr, LV_ALIGN_TOP_LEFT, 8, 78);
    lv_obj_set_style_text_font(open_hdr, &Font_Mono_Bold_14, LV_PART_MAIN);
    lv_label_set_text(open_hdr, "Open in a browser:");

    fs_url_label = lv_label_create(parent);
    lv_obj_set_width(fs_url_label, 224);
    lv_label_set_long_mode(fs_url_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(fs_url_label, LV_ALIGN_TOP_LEFT, 8, 100);
    lv_obj_set_style_text_font(fs_url_label, &Font_Mono_Bold_14, LV_PART_MAIN);
    lv_label_set_text(fs_url_label, "");

    fs_toggle_btn = lv_btn_create(parent);
    lv_obj_set_size(fs_toggle_btn, 200, 40);
    lv_obj_align(fs_toggle_btn, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_set_style_radius(fs_toggle_btn, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(fs_toggle_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(fs_toggle_btn, lv_color_white(), LV_PART_MAIN);
    lv_obj_add_event_cb(fs_toggle_btn, fs_toggle_cb, LV_EVENT_CLICKED, NULL);
    fs_toggle_label = lv_label_create(fs_toggle_btn);
    lv_label_set_text(fs_toggle_label, "Start server");
    lv_obj_set_style_text_color(fs_toggle_label, lv_color_black(), LV_PART_MAIN);
    lv_obj_center(fs_toggle_label);

    fs_hint_label = lv_label_create(parent);
    lv_obj_set_width(fs_hint_label, 224);
    lv_label_set_long_mode(fs_hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(fs_hint_label, LV_ALIGN_TOP_LEFT, 8, 210);
    lv_obj_set_style_text_font(fs_hint_label, &Font_Mono_Bold_14, LV_PART_MAIN);
    lv_label_set_text(fs_hint_label,
        "Browse, download and upload SD files from a device on the same WiFi.");
}

static void fs_entry(void)
{
    /* Serving files needs the network for as long as the screen is open. */
    power_wifi_connect(8000);
    file_server_start();            /* no-op / false if WiFi not up yet */
    fs_update();
    if (!fs_timer) fs_timer = lv_timer_create(fs_poll_cb, 1000, NULL);
    ui_disp_full_refr();
}

static void fs_exit(void)
{
    if (fs_timer) { lv_timer_del(fs_timer); fs_timer = NULL; }
    file_server_stop();
    power_release(PWR_WIFI);
    ui_disp_full_refr();
}

static void fs_destroy(void)
{
    if (fs_timer) { lv_timer_del(fs_timer); fs_timer = NULL; }
    fs_state_label = fs_url_label = fs_hint_label = NULL;
    fs_toggle_btn = fs_toggle_label = NULL;
}

extern "C" {
scr_lifecycle_t screen_fileserver = {
    .create  = fs_create,
    .entry   = fs_entry,
    .exit    = fs_exit,
    .destroy = fs_destroy,
};
}
