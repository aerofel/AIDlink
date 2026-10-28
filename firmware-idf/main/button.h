// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// User key (T-Display-S3 GPIO14), and the touch panel where the board has one
// (touch.c — the whole screen acts as the same key). Hold toggles the preferred
// position source; a short tap raises a transient GPS detail overlay, or the
// uplink detail on a board with no GNSS receiver.
//
// Hold-to-toggle rather than tap-to-toggle is deliberate: a stray press would
// otherwise silently change what is feeding Jeppesen.
//
// Power key (board_t.pwr_btn_gpio, BOOT on the T-Display-S3): hold 1 s and
// release = restart; hold 3 s = power off, which happens on release. Both show
// an overlay while held, so the operator sees which one they are about to get.
#pragma once
#include "config.h"

typedef enum {
    BTN_OVL_NONE = 0,
    BTN_OVL_SOURCE,   // "SOURCE: GPS" / "SOURCE: FEED", ~2 s
    BTN_OVL_DETAIL,   // fix / sats / HDOP / constellations / PPS, ~4 s
    BTN_OVL_LINK,     // uplink SSID / RSSI / internet / AP clients, ~4 s
    BTN_OVL_PWR_RESTART,  // power key held 1-3 s: release restarts
    BTN_OVL_PWR_OFF_HOLD, // held 3 s+: release powers off
    BTN_OVL_PWR_OFF,      // released: powering off now
    BTN_OVL_RESTARTING,   // released: restarting now
} btn_overlay_t;

// Start the poll task. Returns immediately on boards with neither a user key
// nor a working touch panel.
void button_start(aidlink_cfg_t *cfg);

// Currently requested overlay, or BTN_OVL_NONE once it has expired.
btn_overlay_t button_overlay(void);
