// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Board identity: one firmware binary serves every unit; per-board hardware
// (display, WS2812 LED) is looked up here by the chip's factory MAC. Unknown
// boards get the safe generic profile (no display, try the WS2812).
#pragma once
#include <stdbool.h>

typedef struct {
    const char *name;     // e.g. "lilygo-tdisplay-s3"
    bool has_ws2812;      // onboard WS2812 on GPIO48 (S3 devkit)
    bool has_display;     // ST7789 170x320 on the i80 bus (LilyGO T-Display-S3)
} board_t;

// Identify the board from the eFuse base MAC. Never NULL; cached after first call.
const board_t *board_get(void);
