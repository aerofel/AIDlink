// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// User key (T-Display-S3 GPIO14). Hold toggles the preferred position source;
// a short tap raises a transient GPS detail overlay.
//
// Hold-to-toggle rather than tap-to-toggle is deliberate: a stray press would
// otherwise silently change what is feeding Jeppesen.
#pragma once
#include "config.h"

typedef enum {
    BTN_OVL_NONE = 0,
    BTN_OVL_SOURCE,   // "SOURCE: GPS" / "SOURCE: FEED", ~2 s
    BTN_OVL_DETAIL,   // fix / sats / HDOP / constellations / PPS, ~4 s
} btn_overlay_t;

// Start the poll task. Returns immediately on boards with no user key.
void button_start(aidlink_cfg_t *cfg);

// Currently requested overlay, or BTN_OVL_NONE once it has expired.
btn_overlay_t button_overlay(void);
