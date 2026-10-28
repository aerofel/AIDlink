// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Board identity: one firmware source serves every unit; per-board hardware is
// looked up here by the chip's factory MAC and expressed as DRIVERS rather
// than booleans, so adding a board never edits shared logic.
//
// (The one thing that cannot be runtime-selected is the PSRAM interface —
// CONFIG_SPIRAM_MODE_OCT/QUAD is compile-time in ESP-IDF. Boards 2/3 build with
// the default sdkconfig; Board 4 adds sdkconfig.t3s3. Same sources either way.)
#pragma once
#include <stdbool.h>

struct panel_drv_s;
struct layout_drv_s;

typedef enum {
    LED_NONE = 0,
    LED_WS2812,   // addressable RGB (S3 devkit, GPIO48)
    LED_GPIO,     // plain single-colour LED, active high (T3-S3, GPIO37)
} led_kind_t;

typedef enum {
    TOUCH_NONE = 0,
    TOUCH_CST816, // Hynitron CST816 capacitive, I2C 0x15 (T-Display-S3 Touch)
} touch_kind_t;

typedef struct {
    const char *name;                     // e.g. "lilygo-t3s3"
    const char *disp_desc;                // portal text, NULL when headless
    const struct panel_drv_s  *panel;     // NULL = no display on this board
    const struct layout_drv_s *layout;    // NULL = no display on this board
    led_kind_t  led;
    int         led_gpio;
    // Wired GNSS receiver, -1 when this model has none. Pins are a per-board
    // fact, never a shared #define: on the classic ESP32-WROVER GPIO16/17 are
    // the PSRAM lines, so opening a UART on them blind would be destructive.
    // Only 16 (RX) and 12 (TX) are free across the whole fleet; PPS on 21 is
    // Board-3-only (on the T3-S3 it is QWIIC SCL *and* LoRa DIO3).
    int         gps_rx, gps_tx, gps_pps;
    int         btn_gpio;                 // user key, -1 = none
    // Touch controller this model MAY carry; touch.c confirms it at boot.
    // Pins are only read when touch != TOUCH_NONE (rst -1 = not wired).
    touch_kind_t touch;
    int         touch_sda, touch_scl, touch_rst;
    // Li-ion sense input: an ADC1 GPIO behind a 2:1 divider on the JST battery
    // node, -1 when this model has none. A pin here only says the board CAN
    // carry a battery — battery.c decides at runtime whether one is fitted.
    int         bat_adc_gpio;
    // Power key, -1 = none: hold 1 s and release = restart, hold 3 s = power
    // off (deep sleep). Must be an RTC GPIO, since it also wakes the board.
    int         pwr_btn_gpio;
} board_t;

// Identify the board from the eFuse base MAC. Never NULL; cached after the
// first call. Unknown units get a safe headless profile.
const board_t *board_get(void);
