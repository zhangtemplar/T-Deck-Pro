/**
 * @file      test_touchpad.h
 * @author    ShallowGreen123
 * @license   MIT
 * @copyright Copyright (c) 2023  Shenzhen Xin Yuan Electronic Technology Co., Ltd
 * @date      2024-05-27
 *
 */


#include <Arduino.h>
#include "utilities.h"
#include <GxEPD2_BW.h>
#include <TouchDrvCSTXXX.hpp>
#include <TinyGPS++.h>
#include "lvgl.h"
#include "ui_deckpro.h"
#include "cjk_font.h"
#include "file_server.h"
#include "power_mgr.h"
#include "lowpower_mgr.h"
#include "ui_deckpro_port.h"
#include <Fonts/FreeMonoBold9pt7b.h>
#include "factory.h"
#include "peripheral.h"
#include <Preferences.h>
#include <WiFi.h>
#include <freertos/semphr.h>
#include "config_keys.h"

Adafruit_DRV2605 drv;

Preferences preferences;

TinyGsm modem(SerialAT);
TaskHandle_t a7682_handle;

XPowersPPM PPM;
BQ27220 bq27220;
Audio audio;

static constexpr uint16_t FACTORY_BATTERY_DESIGN_CAPACITY_MAH = 1400;
static constexpr uint16_t FACTORY_BQ25896_CHARGE_TARGET_MV = 4208;
static constexpr uint16_t FACTORY_BQ25896_FAST_CHARGE_MA = 512;
static constexpr uint16_t FACTORY_BQ25896_PRECHARGE_MA = 128;
static constexpr uint16_t FACTORY_BQ25896_TERMINATION_MA = 128;
static constexpr uint16_t FACTORY_BQ25896_INPUT_LIMIT_MA = 1000;
static constexpr uint16_t FACTORY_BQ25896_SYS_POWER_DOWN_MV = 3300;
static constexpr uint32_t FACTORY_BQ25896_RUNTIME_CHECK_MS = 5000;
static constexpr uint32_t FACTORY_BQ25896_RECOVERY_COOLDOWN_MS = 30000;
static constexpr uint32_t FACTORY_EPD_SPI_HZ = 2000000;

// TouchDrvCSTXXX touch;
using InkPanel = GxEPD2_310_GDEQ031T10;
using InkDisplay = GxEPD2_BW<InkPanel, InkPanel::HEIGHT>;
static constexpr int16_t BOARD_EPD_RST_UNUSED = -1;

InkDisplay display_v1_1(InkPanel(BOARD_EPD_CS, BOARD_EPD_DC, BOARD_EPD_RST, BOARD_EPD_BUSY)); // GDEQ031T10 240x320, UC8253, (no inking, backside mark KEGMO 3100)
InkDisplay display_v1_0(InkPanel(BOARD_EPD_CS, BOARD_EPD_DC, BOARD_EPD_RST_UNUSED, BOARD_EPD_BUSY));
InkDisplay *display = &display_v1_1;

uint8_t *decodebuffer = NULL;
lv_timer_t *flush_timer = NULL;
int disp_refr_mode = DISP_REFR_MODE_PART;

uint8_t isT_Deck_Pro_v1_1 = 0;
const char Version_str1[] = "T-Deck-Pro V1.0";
const char Version_str2[] = "T-Deck-Pro V1.1";

bool peri_init_st[E_PERI_NUM_MAX] = {0};
static SemaphoreHandle_t shared_spi_mutex = nullptr;

static void shared_spi_release_all_cs()
{
    digitalWrite(BOARD_LORA_CS, HIGH);
    digitalWrite(BOARD_SD_CS, HIGH);
    digitalWrite(BOARD_EPD_CS, HIGH);
}

void shared_spi_bus_init(void)
{
    if (shared_spi_mutex == nullptr) {
        shared_spi_mutex = xSemaphoreCreateRecursiveMutex();
        if (shared_spi_mutex == nullptr) {
            Serial.println("[SPI] Failed to create shared bus mutex");
            return;
        }
    }
    shared_spi_release_all_cs();
}

void shared_spi_lock(void)
{
    if (shared_spi_mutex == nullptr) {
        shared_spi_bus_init();
    }
    if (shared_spi_mutex != nullptr) {
        xSemaphoreTakeRecursive(shared_spi_mutex, portMAX_DELAY);
    }
}

void shared_spi_unlock(void)
{
    if (shared_spi_mutex != nullptr) {
        shared_spi_release_all_cs();
        xSemaphoreGiveRecursive(shared_spi_mutex);
    }
}

void shared_spi_prepare_device(int cs_pin)
{
    shared_spi_release_all_cs();
    if (cs_pin >= 0) {
        digitalWrite(cs_pin, HIGH);
    }
}

/*********************************************************************************
 *                              STATIC PROTOTYPES
 * *******************************************************************************/
static void select_ink_display()
{
    display = isT_Deck_Pro_v1_1 ? &display_v1_1 : &display_v1_0;
    Serial.printf("[EPD] reset mode: %s (RST=%d)\n",
                  isT_Deck_Pro_v1_1 ? "hardware" : "soft-only",
                  isT_Deck_Pro_v1_1 ? BOARD_EPD_RST : BOARD_EPD_RST_UNUSED);
}

static bool ink_screen_init()
{
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_EPD_CS);

    // SPI.begin(BOARD_SPI_SCK, -1, BOARD_SPI_MOSI, BOARD_EPD_CS);
    display->epd2.selectSPI(SPI, SPISettings(FACTORY_EPD_SPI_HZ, MSBFIRST, SPI_MODE0));
    display->init(115200, true, 2, false);
    //Serial.println("helloWorld");
    display->setRotation(0);
    display->setFont(&FreeMonoBold9pt7b);
    if (display->epd2.WIDTH < 104) display->setFont(0);
    display->setTextColor(GxEPD_BLACK);
    int16_t tbx, tby; uint16_t tbw, tbh;
    if(isT_Deck_Pro_v1_1) {
        display->getTextBounds(Version_str2, 0, 0, &tbx, &tby, &tbw, &tbh);
    }
    else {
        display->getTextBounds(Version_str1, 0, 0, &tbx, &tby, &tbw, &tbh);
    }
    // center bounding box by transposition of origin:
    uint16_t x = ((display->width() - tbw) / 2) - tbx;
    uint16_t y = ((display->height() - tbh) / 2) - tby;
    display->setFullWindow();
    display->firstPage();
    do
    {
        display->fillScreen(GxEPD_WHITE);
        display->setCursor(x, y);
        if(isT_Deck_Pro_v1_1) {
            display->print(Version_str2);
        }
        else {
            display->print(Version_str1);
        }

        display->setCursor(x+20, y+20);
        display->print(UI_T_DECK_PRO_VERSION);
    }
    while (display->nextPage());
    // Some board revisions don't wire the panel reset line, so avoid deep sleep.
    display->powerOff();
    shared_spi_unlock();
    return true;
}

static void convert_lvgl_buf_to_epd_bitmap(const lv_color_t *color_p, lv_coord_t width, lv_coord_t height)
{
    const size_t stride = EPD_BITMAP_STRIDE(width);
    const size_t bitmap_size = stride * size_t(height);

    memset(decodebuffer, 0xFF, bitmap_size);

    for (lv_coord_t y = 0; y < height; ++y) {
        size_t row_offset = size_t(y) * stride;
        for (lv_coord_t x = 0; x < width; ++x) {
            const size_t pixel_index = size_t(y) * size_t(width) + size_t(x);
            if (lv_color_brightness(color_p[pixel_index]) < 128) {
                decodebuffer[row_offset + size_t(x / 8)] &= ~(0x80 >> (x & 0x7));
            }
        }
    }
}

static void flush_epd_bitmap(const lv_area_t *area)
{
    const lv_coord_t width = lv_area_get_width(area);
    const lv_coord_t height = lv_area_get_height(area);

    if ((width <= 0) || (height <= 0)) {
        return;
    }

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_EPD_CS);

    if (disp_refr_mode == DISP_REFR_MODE_PART) {
        display->setPartialWindow(area->x1, area->y1, width, height);
    } else {
        display->setFullWindow();
    }

    display->firstPage();
    do {
        if (disp_refr_mode == DISP_REFR_MODE_FULL) {
            display->fillScreen(GxEPD_WHITE);
        }
        display->drawInvertedBitmap(area->x1, area->y1, decodebuffer, width, height, GxEPD_BLACK);
    }
    while (display->nextPage());

    display->powerOff();
    shared_spi_unlock();
}

/* Hold the panel update off briefly while keys are still arriving. A refresh
 * blocks for a few hundred ms and nothing else runs during it, so refreshing
 * per keystroke makes typing feel like one character per screen update. Pausing
 * LVGL's refresh timer turns a burst of keys into a single refresh; the
 * deadline guarantees the screen still updates during sustained typing.
 *
 * This throttles the refresh timer rather than the flush itself: flush_cb has
 * to complete once LVGL calls it, and flush_timer_cb below never runs (the
 * driver's render_start_cb is commented out where it is registered). */
#define EPD_TYPING_GAP_MS   90
#define EPD_TYPING_MAX_MS   700

static void epd_typing_throttle(void)
{
    lv_disp_t *disp = lv_disp_get_default();
    if(disp == NULL || disp->refr_timer == NULL) return;

    static uint32_t hold_since = 0;
    uint32_t now = millis();

    if(now - keypad_last_activity_ms() < EPD_TYPING_GAP_MS) {
        if(hold_since == 0) hold_since = now;
        if(now - hold_since < EPD_TYPING_MAX_MS) {
            lv_timer_pause(disp->refr_timer);
            return;
        }
    }
    hold_since = 0;
    lv_timer_resume(disp->refr_timer);
}

static void flush_timer_cb(lv_timer_t *t)
{
    static int idx = 0;
    lv_disp_t *disp = lv_disp_get_default();
    if(disp->rendering_in_progress == false) {
        lv_area_t full_area;
        full_area.x1 = 0;
        full_area.y1 = 0;
        full_area.x2 = LCD_HOR_SIZE - 1;
        full_area.y2 = LCD_VER_SIZE - 1;

        flush_epd_bitmap(&full_area);
        
        Serial.printf("flush_timer_cb:%d, %s\n", idx++, (disp_refr_mode == DISP_REFR_MODE_FULL ? "full" : "part"));

        disp_refr_mode = DISP_REFR_MODE_PART;
        lv_timer_pause(flush_timer);
    }
}

static void dips_render_start_cb(struct _lv_disp_drv_t * disp_drv)
{
    if(flush_timer == NULL) {
        flush_timer = lv_timer_create(flush_timer_cb, 10, NULL);
    } else {
        lv_timer_resume(flush_timer);
    }
}

static void disp_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p)
{
    convert_lvgl_buf_to_epd_bitmap(color_p, lv_area_get_width(area), lv_area_get_height(area));
    flush_epd_bitmap(area);
    
    static int idx = 0;
    Serial.printf("disp_flush:%d, %s, area=(%d,%d)-(%d,%d)\n",
                  idx++,
                  (disp_refr_mode == DISP_REFR_MODE_FULL ? "full" : "part"),
                  area->x1, area->y1, area->x2, area->y2);

    disp_refr_mode = DISP_REFR_MODE_PART;

    // Serial.printf("x1=%d, y1=%d, x2=%d, y2=%d\n", area->x1, area->y1, area->x2, area->y2);

    /*IMPORTANT!!!
     *Inform the graphics library that you are ready with the flushing*/
    lv_disp_flush_ready(disp_drv);
}

static void touchpad_read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data)
{
    static lv_coord_t last_x = 0;
    static lv_coord_t last_y = 0;

    // uint8_t touched = touch.getPoint(&last_x, &last_y, 1);
    uint8_t touched = hyn_touch_get_point(&last_x, &last_y, 1);
    if(touched) {
        data->state = LV_INDEV_STATE_PR;
        lowpower_note_activity();

        Serial.printf("x = %d, y = %d\n", last_x, last_y);
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
    /*Set the last pressed coordinates*/
    data->point.x = last_x;
    data->point.y = last_y;
}

static void lvgl_init(void)
{
    lv_init();

    static lv_disp_draw_buf_t draw_buf_dsc_1;
    lv_color_t *buf_1 = (lv_color_t *)ps_calloc(sizeof(lv_color_t), DISP_BUF_SIZE);
    lv_color_t *buf_2 = (lv_color_t *)ps_calloc(sizeof(lv_color_t), DISP_BUF_SIZE);
    lv_disp_draw_buf_init(&draw_buf_dsc_1, buf_1, buf_2, LCD_HOR_SIZE * LCD_VER_SIZE);
    decodebuffer = (uint8_t *)ps_calloc(sizeof(uint8_t), EPD_BITMAP_BUF_SIZE);
    // lv_disp_draw_buf_init(&draw_buf, lv_disp_buf_p, NULL, DISP_BUF_SIZE);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_HOR_SIZE;
    disp_drv.ver_res = LCD_VER_SIZE;
    disp_drv.flush_cb = disp_flush;
    // disp_drv.render_start_cb = dips_render_start_cb;
    disp_drv.draw_buf = &draw_buf_dsc_1;
    // disp_drv.rounder_cb = display_driver_rounder_cb;
    disp_drv.full_refresh = 1;

    lv_disp_drv_register(&disp_drv);

    /*------------------
     * Touchpad
     * -----------------*/
    /*Register a touchpad input device*/
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touchpad_read;
    lv_indev_drv_register(&indev_drv);
}

void ink_screen_prepare_shutdown(void)
{
    if (!peri_init_st[E_PERI_INK_SCREEN]) {
        return;
    }

    digitalWrite(BOARD_EPD_BL, LOW);
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_EPD_CS);
    display->powerOff();
    shared_spi_unlock();
}

static bool bq25896_apply_factory_profile(void)
{
    PPM.resetDefault();
    PPM.disableWatchdog();
    PPM.exitHizMode();
    PPM.disableOTG();
    PPM.enableBatterPowerPath();
    PPM.setInputCurrentLimit(FACTORY_BQ25896_INPUT_LIMIT_MA);
    PPM.setSysPowerDownVoltage(FACTORY_BQ25896_SYS_POWER_DOWN_MV);
    PPM.setChargeTargetVoltage(FACTORY_BQ25896_CHARGE_TARGET_MV);
    PPM.setChargerConstantCurr(FACTORY_BQ25896_FAST_CHARGE_MA);
    PPM.setPrechargeCurr(FACTORY_BQ25896_PRECHARGE_MA);
    PPM.setTerminationCurr(FACTORY_BQ25896_TERMINATION_MA);
    PPM.enableChargingTermination();
    PPM.enableCharge();
    return PPM.enableMeasure();
}

static bool bq25896_init(void)
{
    // BQ25896 --- 0x6B
    Wire.beginTransmission(BOARD_I2C_ADDR_BQ25896);
    if (Wire.endTransmission() == 0)
    {
        if (!PPM.init(Wire, BOARD_I2C_SDA, BOARD_I2C_SCL, BOARD_I2C_ADDR_BQ25896)) {
            return false;
        }

        return bq25896_apply_factory_profile();
    }
    return false;
}

static bool bq27220_init(void)
{
    bq27220.setDefaultCapacity(FACTORY_BATTERY_DESIGN_CAPACITY_MAH);
    bool ret = bq27220.init();
    // if(ret) 
    //     bq27220.reset();
    return ret;
}

static void bq25896_runtime_maintain(void)
{
    static uint32_t last_check_ms = 0;
    static uint32_t last_recovery_ms = 0;

    if (!peri_init_st[E_PERI_BQ25896]) {
        return;
    }
    if (millis() - last_check_ms < FACTORY_BQ25896_RUNTIME_CHECK_MS) {
        return;
    }
    last_check_ms = millis();

    if (!PPM.isVbusIn()) {
        return;
    }

    bool need_recover = !PPM.isCharging();
    if (!need_recover && peri_init_st[E_PERI_BQ27220]) {
        need_recover = (bq27220.getAverageCurrent() < 0);
    }
    if (!need_recover) {
        return;
    }
    if (millis() - last_recovery_ms < FACTORY_BQ25896_RECOVERY_COOLDOWN_MS) {
        return;
    }

    Serial.println("[BQ25896] Restore charge path");
    bq25896_apply_factory_profile();
    last_recovery_ms = millis();
}

static bool sd_care_init(void)
{
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);

    if(!SD.begin(BOARD_SD_CS, SPI)){
        shared_spi_unlock();
        Serial.println("[SD CARD] Card Mount Failed");
        return false;
    }

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("SD Card Size: %lluMB\n", cardSize);

    uint64_t totalSize = SD.totalBytes() / (1024 * 1024);
    Serial.printf("SD Card Total: %lluMB\n", totalSize);

    uint64_t usedSize = SD.usedBytes() / (1024 * 1024);
    Serial.printf("SD Card Used: %lluMB\n", usedSize);
    shared_spi_unlock();
    return true;
}

static void a7682_task(void *param)
{
    vTaskSuspend(a7682_handle);
    while (1)
    {
        while (SerialAT.available())
        {
            SerialMon.write(SerialAT.read());
        }
        while (SerialMon.available())
        {
            SerialAT.write(SerialMon.read());
        }
        delay(1);
    }
}

static bool A7682E_init(void)
{
    Serial.println("Place your board outside to catch satelite signal");

    // Set module baud rate and UART pins
    SerialAT.begin(115200, SERIAL_8N1, BOARD_A7682E_TXD, BOARD_A7682E_RXD);

    Serial.println("Start modem...");

    // power on
    digitalWrite(BOARD_A7682E_PWRKEY, LOW);
    delay(10);
    digitalWrite(BOARD_A7682E_PWRKEY, HIGH);
    delay(50);
    digitalWrite(BOARD_A7682E_PWRKEY, LOW);
    delay(10);

    int retry_cnt = 5;
    int retry = 0;
    while (!modem.testAT(1000)) {
        Serial.println(".");
        if (retry++ > retry_cnt) {
            digitalWrite(BOARD_A7682E_PWRKEY, LOW);
            delay(100);
            digitalWrite(BOARD_A7682E_PWRKEY, HIGH);
            delay(1000);
            digitalWrite(BOARD_A7682E_PWRKEY, LOW);

            Serial.println("[A7682E] Init Fail");
            break;
        }
    }
    
    Serial.println();
    delay(200);

    xTaskCreate(a7682_task, "a7682_handle", 1024 * 3, NULL, A7682E_PRIORITY, &a7682_handle);

    return (retry < retry_cnt);
}

bool pcm5102a_init(void)
{
    bool ret = audio.setPinout(BOARD_I2S_BCLK, BOARD_I2S_LRC, BOARD_I2S_DOUT);

    if (ret == false) 
        Serial.printf("[%d] Execution error\n", __LINE__);

    audio.setVolume(21); // 0...21

    pinMode(BOARD_6609_EN, OUTPUT);
    digitalWrite(BOARD_6609_EN, HIGH);

    // audio_paly_flag = audio.connecttoFS(SD, "/voice_time/BBIBBI.mp3");

    return true;
}

static void listDir(fs::FS &fs, const char * dirname, uint8_t levels){
    Serial.printf("Listing spiffs directory: %s\n", dirname);

    File root = fs.open(dirname);
    if(!root){
        Serial.println("- failed to open directory");
        return;
    }
    if(!root.isDirectory()){
        Serial.println(" - not a directory");
        return;
    }

    File file = root.openNextFile();
    while(file){
        if(file.isDirectory()){
            Serial.print("  DIR : ");
            Serial.println(file.name());
            if(levels){
                listDir(fs, file.path(), levels -1);
            }
        } else {
            Serial.print("  FILE: ");
            Serial.print(file.name());
            Serial.print("\tSIZE: ");
            Serial.println(file.size());
        }
        file = root.openNextFile();
    }
}


/* Days-from-civil, so GPS UTC can seed the clock without timegm(). */
static time_t utc_fields_to_epoch(int y, int mo, int d, int h, int mi, int sec)
{
    y -= (mo <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + (long)doe - 719468L;
    return (time_t)days * 86400 + h * 3600 + mi * 60 + sec;
}

/* One short window at boot with WiFi and GPS up, then both back down.
 *
 * NTP is the reliable source and usually answers in a second or two. GPS runs
 * alongside it because it also carries UTC (useful where there is no WiFi) and
 * because a position fix here gets cached for the weather app. Neither is
 * allowed to extend boot: the whole thing is bounded by the NTP wait. */
static void boot_time_sync(void)
{
    /* NTP first, on its own. GPS is only worth powering here if the network
     * couldn't set the clock — otherwise it would run (and log) for the whole
     * linger window after every boot for no benefit. */
    bool wifi = power_wifi_connect(8000);

    bool have_time = false;
    if (wifi) {
        struct tm tm;
        if (getLocalTime(&tm, 5000)) {
            have_time = true;
            Serial.printf("[TIME] NTP synced %04d-%02d-%02d %02d:%02d:%02d\n",
                          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                          tm.tm_hour, tm.tm_min, tm.tm_sec);
        } else {
            Serial.println("[TIME] NTP did not answer in time");
        }
    }

    /* Fall back to GPS time when the network was no help. Powering the
     * receiver now also gives it a chance to cache a fix for the weather app
     * during its warm window. */
    if (!have_time) {
        power_acquire(PWR_GPS);
        uint16_t yr; uint8_t mo, dy, hh, mm, ss;
        ui_gps_get_data(&yr, &mo, &dy);
        ui_gps_get_time(&hh, &mm, &ss);
        if (yr >= 2020) {
            struct timeval tv = { .tv_sec = utc_fields_to_epoch(yr, mo, dy, hh, mm, ss),
                                  .tv_usec = 0 };
            settimeofday(&tv, NULL);
            have_time = true;
            Serial.printf("[TIME] GPS synced %04u-%02u-%02u %02u:%02u:%02u UTC\n",
                          yr, mo, dy, hh, mm, ss);
        }
    }
    if (!have_time) Serial.println("[TIME] clock not set this boot");

    /* Cache a fix if we happened to get one; the weather app reads this. */
    double lat = 0, lng = 0;
    ui_gps_get_coord(&lat, &lng);
    if (lat != 0 || lng != 0) {
        Preferences p;
        p.begin("weather", false);
        p.putFloat("gps_lat", (float)lat);
        p.putFloat("gps_lon", (float)lng);
        p.end();
        Serial.printf("[TIME] cached GPS fix %.4f,%.4f for weather\n", lat, lng);
    }

    if (!have_time) power_release(PWR_GPS);   /* only taken on the NTP-failed path */
    power_release(PWR_WIFI);
}

void setup()
{
    gpio_hold_dis((gpio_num_t)BOARD_6609_EN);
    gpio_hold_dis((gpio_num_t)BOARD_LORA_EN);
    gpio_hold_dis((gpio_num_t)BOARD_GPS_EN);
    gpio_hold_dis((gpio_num_t)BOARD_A7682E_PWRKEY);

    gpio_deep_sleep_hold_dis();

    Serial.begin(115200);

    // IO
    pinMode(BOARD_KEYBOARD_LED, OUTPUT);
    pinMode(BOARD_MOTOR_PIN, OUTPUT);
    pinMode(BOARD_6609_EN, OUTPUT);         // enable 7682 module
    pinMode(BOARD_LORA_EN, OUTPUT);         // enable LORA module
    pinMode(BOARD_GPS_EN, OUTPUT);          // enable GPS module
    pinMode(BOARD_A7682E_PWRKEY, OUTPUT); 
    digitalWrite(BOARD_KEYBOARD_LED, LOW);
    digitalWrite(BOARD_MOTOR_PIN, HIGH);
    digitalWrite(BOARD_6609_EN, HIGH);
    digitalWrite(BOARD_LORA_EN, HIGH);
    digitalWrite(BOARD_GPS_EN, HIGH);
    digitalWrite(BOARD_A7682E_PWRKEY, HIGH);

    pinMode(BOARD_EPD_BL, OUTPUT); 
    digitalWrite(BOARD_EPD_BL, HIGH);

    // LORA、SD、EPD use the same SPI, in order to avoid mutual influence;
    // before powering on, all CS signals should be pulled high and in an unselected state;
    pinMode(BOARD_LORA_CS, OUTPUT); 
    digitalWrite(BOARD_LORA_CS, HIGH);
    pinMode(BOARD_LORA_RST, OUTPUT); 
    digitalWrite(BOARD_LORA_RST, HIGH);
    pinMode(BOARD_SD_CS, OUTPUT); 
    digitalWrite(BOARD_SD_CS, HIGH);
    pinMode(BOARD_EPD_CS, OUTPUT); 
    digitalWrite(BOARD_EPD_CS, HIGH);
    shared_spi_bus_init();

    // i2c devices
    byte error, address;
    int nDevices = 0;
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
    Serial.printf(" ------------- I2C ------------- \n");
    for(address = 0x01; address < 0x7F; address++){
        Wire.beginTransmission(address);
        error = Wire.endTransmission();
        if(error == 0){ // 0: success.
            nDevices++;
            if(address == BOARD_I2C_ADDR_TOUCH){
                // flag_Touch_init = true;
                Serial.printf("[0x%x] TOUCH find!\n", address);
            } else if (address == BOARD_I2C_ADDR_GYROSCOPDE) {
                Serial.printf("[0x%x] GYROSCOPDE find!\n", address);
            } else if (address == BOARD_I2C_ADDR_KEYBOARD) {
                Serial.printf("[0x%x] KEYBOARD find!\n", address);
            } else if (address == BOARD_I2C_ADDR_BQ27220) {
                Serial.printf("[0x%x] BQ27220 find!\n", address);
            } else if (address == BOARD_I2C_ADDR_BQ25896) {
                Serial.printf("[0x%x] BQ25896 find!\n", address);
            } else if (address == BOARD_I2C_ADDR_DRV2605) {
                Serial.printf("[0x%x] DRV2605 find!\n", address);
            }
        }
    }

    for(int i = 0; i < 3; i++) {
        Wire.beginTransmission(BOARD_I2C_ADDR_DRV2605);
        error = Wire.endTransmission();
        if(error == 0) {
            isT_Deck_Pro_v1_1 = 1;
        } else {
            isT_Deck_Pro_v1_1 = 0;
        }
    }

    select_ink_display();

    if(isT_Deck_Pro_v1_1) {
        Serial.println("Adafruit DRV2605 Basic test");
        if (!drv.begin()) {
            Serial.println("Could not find DRV2605");
            while (1) delay(10);
        }
        
        drv.selectLibrary(1);
        
        // I2C trigger by sending 'go' command 
        // default, internal trigger when sending GO command
        drv.setMode(DRV2605_MODE_INTTRIG); 
    }

#ifdef T_DECK_PRO_V1_1
    if(!isT_Deck_Pro_v1_1){
        Serial.printf(" ------------- WARN ------------- \n");
        Serial.printf("Detected T-Deck-Pro V1.0 hardware\n");
        Serial.printf("EPD compatibility mode enabled: panel reset is disabled for stable refresh.\n");
        Serial.printf("If other peripherals behave differently, prefer H693_factory_v1.x.bin.\n");
    }
#endif

    Serial.printf(" ------------- SPIFFS ------------- \n");

    if(!SPIFFS.begin(true)){
        Serial.println("SPIFFS Mount Failed");
        return;
    }

    listDir(SPIFFS, "/", 0);
    Serial.println(" ------------- PERI ------------- ");

    // SPI
    SPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI);

    // init peripheral
    peri_init_st[E_PERI_INK_SCREEN] = ink_screen_init();
    peri_init_st[E_PERI_LORA]       = lora_init();
    peri_init_st[E_PERI_KYEPAD]     = keypad_init(BOARD_I2C_ADDR_KEYBOARD);
    peri_init_st[E_PERI_BQ25896]    = bq25896_init();
    peri_init_st[E_PERI_BQ27220]    = bq27220_init();
    peri_init_st[E_PERI_SD]         = sd_care_init();
    peri_init_st[E_PERI_GPS]        = gps_init();
    peri_init_st[E_PERI_BHI260AP]   = BHI260AP_init();
    peri_init_st[E_PERI_A7682E]     = A7682E_init();

    if(peri_init_st[E_PERI_A7682E] == false)
    {
        peri_init_st[E_PERI_PCM5102A] = pcm5102a_init();
    }

    peri_init_st[E_PERI_TOUCH] = hyn_touch_init();

    lvgl_init();

    /* SD is mounted (sd_care_init above) and LVGL is up: load the streaming
     * CJK font and install it as the theme font before any screen is created. */
    cjk_font_init();

    ui_deckpro_entry();

    disp_full_refr();

    digitalWrite(BOARD_KEYBOARD_LED, LOW);
    digitalWrite(BOARD_MOTOR_PIN, HIGH);

    /* Radios stay off until an app asks for them (power_mgr.h). Leaving WiFi
     * associated and the GPS/LoRa/modem rails up was costing most of the
     * battery, since the device spends nearly all its time showing a static
     * page on a panel that needs no power to hold an image. */
    configTzTime("PST8PDT,M3.2.0,M11.1.0", "pool.ntp.org");
    power_mgr_init();

    boot_time_sync();
    lowpower_init();
    power_log_state("boot done");
}


uint32_t tick = 0;

void loop()
{
    epd_typing_throttle();
    lv_task_handler();
    keypad_loop();
    extern void calc_keyboard_poll();
    calc_keyboard_poll();
    extern void weather_keyboard_poll();
    weather_keyboard_poll();
    extern void calendar_keyboard_poll();
    calendar_keyboard_poll();
    extern void dict_keyboard_poll();
    dict_keyboard_poll();
    extern void gps_keyboard_poll();
    gps_keyboard_poll();
    extern void voiceai_keyboard_poll();
    voiceai_keyboard_poll();
    extern void recorder_keyboard_poll();
    recorder_keyboard_poll();
    extern void image_keyboard_poll();
    image_keyboard_poll();
    extern void reader_keyboard_poll();
    reader_keyboard_poll();
    extern void notes_keyboard_poll();
    notes_keyboard_poll();
    file_server_loop();
    power_mgr_tick();      /* expire warm-linger windows (GPS) */
    lowpower_poll();
    bq25896_runtime_maintain();

    audio.loop();

    /* Idle longer when throttled — the keypad is still polled every pass, so
     * this only costs a few ms of key latency. */
    delay(lowpower_is_idle() ? 20 : 1);


    if(millis() - tick > 3000) {
        tick = millis();
        // printf("BOARD_LORA_CS=%d\n", digitalRead(BOARD_LORA_CS));
        // printf("BOARD_LORA_RST=%d\n", digitalRead(BOARD_LORA_RST));
        // printf("BOARD_LORA_BUSY=%d\n", digitalRead(BOARD_LORA_BUSY));
        // printf("BOARD_LORA_EN=%d\n", digitalRead(BOARD_LORA_EN));

        // printf("BOARD_EPD_CS=%d\n", digitalRead(BOARD_EPD_CS));
    }
}

/*********************************************************************************
 *                              GLOBAL PROTOTYPES
 * *******************************************************************************/
void disp_full_refr(void)
{
    disp_refr_mode = DISP_REFR_MODE_FULL;
}


