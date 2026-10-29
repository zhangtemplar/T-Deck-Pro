/**
 * @file      utilities.h
 * @author    ShallowGreen123
 * @license   MIT
 * @copyright Copyright (c) 2023  Shenzhen Xin Yuan Electronic Technology Co., Ltd
 * @date      2024-05-27
 */
#pragma once

#define T_DECK_PRO_V1_1 1

#define UI_T_DECK_PRO_VERSION    "v2.4-260320"  // Software version
#define BOARD_T_DECK_PRO_VERSION "v2.0-250915"  // Hardware version

/* T-Deck-Pro is sold in two mutually exclusive accessory variants:
 *   - 4G version: A7682E modem. GPIO7/8/9 are modem sideband pins.
 *   - non-4G version: PCM5102A DAC. GPIO7/8/9 are I2S BCLK/DOUT/LRC.
 *
 * The schematic names are left below for both variants because many upstream
 * examples do the same, but firmware must only drive the side that is actually
 * populated on the board. PlatformIO selects the variant; plain Arduino builds
 * keep the historical A7682E default.
 */
#if !defined(T_DECK_PRO_HAS_A7682E) && !defined(T_DECK_PRO_HAS_PCM5102A)
#define T_DECK_PRO_HAS_A7682E 1
#endif

#ifndef T_DECK_PRO_HAS_A7682E
#define T_DECK_PRO_HAS_A7682E 0
#endif
#ifndef T_DECK_PRO_HAS_PCM5102A
#define T_DECK_PRO_HAS_PCM5102A 0
#endif

#if T_DECK_PRO_HAS_A7682E && T_DECK_PRO_HAS_PCM5102A
#error "Select only one T-Deck-Pro accessory variant: A7682E or PCM5102A."
#endif
#if !T_DECK_PRO_HAS_A7682E && !T_DECK_PRO_HAS_PCM5102A
#error "Select a T-Deck-Pro accessory variant: A7682E or PCM5102A."
#endif

#if T_DECK_PRO_HAS_A7682E
#define T_DECK_PRO_ACCESSORY_NAME "A7682E 4G"
#define T_DECK_PRO_ACCESSORY_POWER_LABEL "Power A7682"
#else
#define T_DECK_PRO_ACCESSORY_NAME "PCM5102A audio"
#define T_DECK_PRO_ACCESSORY_POWER_LABEL "Power Audio"
#endif

// Serial
#define SerialMon   Serial      // 
#define SerialAT    Serial1     // 
#define SerialGPS   Serial2     // 

// IIC Addr
#define BOARD_I2C_ADDR_TOUCH      0x1A // Touch        --- CST328
#define BOARD_I2C_ADDR_GYROSCOPDE 0x28 // Gyroscope    --- BHI260AP
#define BOARD_I2C_ADDR_KEYBOARD   0x34 // Keyboard     --- TCA8418
#define BOARD_I2C_ADDR_BQ27220    0x55 //
#define BOARD_I2C_ADDR_DRV2605    0x5A // Motor
#define BOARD_I2C_ADDR_BQ25896    0x6B //

// IIC
#define BOARD_I2C_SDA  13
#define BOARD_I2C_SCL  14

#define BOARD_I2S_BCLK 7
#define BOARD_I2S_DOUT 8
#define BOARD_I2S_LRC 9
/* I2S TX cushion for PCM5102A. E-paper refresh can block the foreground loop
 * for a few hundred ms; a larger DMA queue lets already-decoded PCM continue
 * while LVGL updates progress text. */
#define BOARD_AUDIO_DMA_BUF_COUNT 32
#define BOARD_AUDIO_DMA_BUF_LEN   1024
/* ESP32-audioI2S compressed input buffer. PSRAM is used when available; RAM is
 * only the fallback. This must be configured before the first connecttoFS(). */
#define BOARD_AUDIO_INPUT_BUF_RAM   (32 * 1024)
#define BOARD_AUDIO_INPUT_BUF_PSRAM (768 * 1024)

// Keyboard
#define BOARD_KEYBOARD_SCL BOARD_I2C_SCL
#define BOARD_KEYBOARD_SDA BOARD_I2C_SDA
#define BOARD_KEYBOARD_INT 15
#define BOARD_KEYBOARD_LED 42

// Touch
#define BOARD_TOUCH_SCL BOARD_I2C_SCL
#define BOARD_TOUCH_SDA BOARD_I2C_SDA
#define BOARD_TOUCH_INT 12
#define BOARD_TOUCH_RST 38

// Gyroscope
#define BOARD_GYROSCOPDE_SCL BOARD_I2C_SCL
#define BOARD_GYROSCOPDE_SDA BOARD_I2C_SDA
#define BOARD_GYROSCOPDE_INT 21
#define BOARD_GYROSCOPDE_RST -1

// SPI
#define BOARD_SPI_SCK  36
#define BOARD_SPI_MOSI 33
#define BOARD_SPI_MISO 47

// Display
#define LCD_HOR_SIZE 240
#define LCD_VER_SIZE 320
#define DISP_BUF_SIZE (LCD_HOR_SIZE * LCD_VER_SIZE)

#define BOARD_EPD_SCK  BOARD_SPI_SCK
#define BOARD_EPD_MOSI BOARD_SPI_MOSI
#define BOARD_EPD_DC   35
#define BOARD_EPD_CS   34
#define BOARD_EPD_BUSY 37
#define BOARD_EPD_RST  16
#define BOARD_EPD_BL   45  // screen backlight

// SD card
#define BOARD_SD_CS   48
#define BOARD_SD_SCK  BOARD_SPI_SCK
#define BOARD_SD_MOSI BOARD_SPI_MOSI
#define BOARD_SD_MISO BOARD_SPI_MISO

// Lora
#define BOARD_LORA_SCK  BOARD_SPI_SCK
#define BOARD_LORA_MOSI BOARD_SPI_MOSI
#define BOARD_LORA_MISO BOARD_SPI_MISO
#define BOARD_LORA_CS   3
#define BOARD_LORA_BUSY 6
#define BOARD_LORA_RST  4
#define BOARD_LORA_INT  5

// GPS
#define BOARD_GPS_RXD 44
#define BOARD_GPS_TXD 43
#define BOARD_GPS_PPS 1

// A7682E Modem
#define BOARD_A7682E_RI     7
#define BOARD_A7682E_ITR    8
#define BOARD_A7682E_RST    9
#define BOARD_A7682E_RXD    10
#define BOARD_A7682E_TXD    11
#define BOARD_A7682E_PWRKEY 40

// Boot pin
#define BOARD_BOOT_PIN  0

// Motor pin
#define BOARD_MOTOR_PIN 2

// EN
#define BOARD_GPS_EN  39  // enable GPS module
#define BOARD_6609_EN 41  // accessory rail: A7682E on 4G boards, PCM5102A on non-4G boards
#define BOARD_LORA_EN 46  // enable LORA module

// Mic
#define BOARD_MIC_DATA        17
#define BOARD_MIC_CLOCK       18
// -------------------------------------------------


