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
} board_t;

// Identify the board from the eFuse base MAC. Never NULL; cached after the
// first call. Unknown units get a safe headless profile.
const board_t *board_get(void);
