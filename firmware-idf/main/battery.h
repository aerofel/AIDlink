// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Battery gauge for boards whose profile declares a sense pin (board_t
// .bat_adc_gpio). Everywhere else every call is a cheap no-op and the gauge
// reports "not present", so the display and portal look exactly as before.
//
// The sense rail only shows the cell while running on battery (batgauge.h), so
// "a battery is fitted" is LEARNED: it is remembered in NVS the first time the
// board is seen running on its battery, and forgotten when the board cold-boots
// from USB (a fitted, charged cell would have kept it powered) or when the
// portal's "no battery fitted" button is pressed.
#pragma once
#include <stdbool.h>
#include "batgauge.h"
#include "batstats.h"

typedef struct {
    bool           supported;   // board has a sense pin and the ADC came up
    battery_t      g;           // what the display shows
    batstats_out_t s;           // portal figures
    int            cap_mah;     // configured capacity, 0 = unknown
} battery_info_t;

// Set up the ADC. Safe to call on any board.
void battery_start(void);
// Take one reading. Call every ~100 ms from ONE task (the display task does).
void battery_poll(void);
// Latest display state, from any task.
void battery_get(battery_t *out);
// Everything the portal panel shows, from any task.
void battery_info(battery_info_t *out);
// True when this board has a sense pin and the ADC came up.
bool battery_supported(void);
// Portal actions: capacity in mAh (0 = unknown), and "no battery fitted".
void battery_set_capacity(int mah);
void battery_forget(void);
