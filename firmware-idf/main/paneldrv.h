// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// The two display seams.
//
//   panel_drv_t   owns HARDWARE bring-up only: bus, controller, geometry.
//                 It knows nothing about flights.
//   layout_drv_t  owns PIXELS only: which widget sits where, in what font.
//                 It knows nothing about I2C, i80 buses or panel controllers.
//
// Between them sits flightview_t (see flightview.h), which owns the numbers.
// A new board therefore costs one panel file (if the controller is new) plus
// one layout file, and touches no shared logic.
#pragma once
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3

#include <stdbool.h>
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "flightview.h"

typedef struct panel_drv_s {
    const char *name;
    // Bring the panel up and fill *cfg (io + panel handles, geometry, colour
    // format) ready for lvgl_port_add_disp(). Return false on any failure —
    // display_start() then leaves the board headless rather than half-driven.
    // Must leave the backlight OFF: panel RAM survives resets, so lighting it
    // before the first flush shows whatever the previous firmware left there.
    bool (*init)(lvgl_port_display_cfg_t *cfg);
    // Called once after the first frame has been rendered. Backlight on, etc.
    void (*ready)(void);
} panel_drv_t;

typedef struct layout_drv_s {
    const char *name;
    void (*build)(lv_display_t *disp);        // construct the widget tree
    void (*render)(const flightview_t *v);    // slow content, ~2 Hz
    void (*status)(const fv_status_t *s);     // indicators, every 100 ms tick
} layout_drv_t;

// Board 3 — LilyGO T-Display-S3: ST7789 320x170 on the i80 bus.
extern const panel_drv_t  panel_st7789;
extern const layout_drv_t layout_tdisplay;

// Board 4 — LilyGO T3-S3: SSD1306 128x64 mono on I2C.
extern const panel_drv_t  panel_ssd1306;
extern const layout_drv_t layout_oled;

#endif // CONFIG_IDF_TARGET_ESP32S3
