// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Li-ion gauge estimator: pure logic, no hardware, host-testable
// (host_test/test_batgauge.c). battery.c feeds it one ADC reading every
// ~100 ms and decides what the screen shows.
//
// What the sense pin really measures (LilyGO T-Display-S3 schematic, and
// confirmed on Board 6 on 2026-10-28): the 100k/100k divider sits on the
// switched SYSTEM rail, not on the cell. That rail is fed either from VBUS
// through a 1N5819 Schottky (~4.7 V on USB, battery or not) or from the cell
// through a P-MOSFET when USB is absent. So:
//   - on USB the reading says "external power" and nothing about the cell;
//   - on battery it is the cell voltage, and the percentage is real.
// The TP4065's charge-status pin only drives an LED, so "charging" can only
// ever mean "a battery is fitted and USB is present".
#pragma once
#include <stdbool.h>
#include <stdint.h>

// What the layouts consume (fv_status_t.battery).
typedef struct {
    bool present;    // show a gauge at all
    bool ext_power;  // USB power present
    bool charging;   // present && ext_power
    int  pct;        // 0..100 measured on battery; -1 = unknown (on USB)
    int  mv;         // sense-rail millivolts, divider undone (diagnostics)
    int  ripple_mv;  // p80-p20 spread of the last window (diagnostics)
} battery_t;

// What the estimator can actually tell from the rail.
typedef enum {
    BATG_UNKNOWN = 0,   // not enough samples yet, or in transition
    BATG_ON_BATTERY,    // steady cell-range voltage: running from the battery
    BATG_ON_USB,        // USB rail present (or a USB host has us mounted)
} batg_state_t;

#define BATG_WIN 30          // samples per window (3 s at 10 Hz)

typedef struct {
    int16_t      ring[BATG_WIN];
    int          n, idx, tick;
    int          batt_run, usb_run;
    int32_t      v_q4;       // smoothed cell mV x16 while on battery; 0 = unset
    batg_state_t state;
    int          pct;        // valid while state == BATG_ON_BATTERY
    int          med_mv, ripple_mv;
} batgauge_t;

void batgauge_init(batgauge_t *g);
// One rail reading in mV (divider already undone). usb_host is true while a
// USB host has the device configured and the bus is not suspended; it only
// tips a borderline rail towards USB (see HOST_MIN_MV), never overrides it.
void batgauge_sample(batgauge_t *g, int mv, bool usb_host);

// Loaded-cell voltage -> state of charge, piecewise linear (exposed for tests).
int batgauge_pct_from_mv(int mv);
// Same curve in tenths of a percent, for rate estimates finer than 1 %.
int batgauge_pct10_from_mv(int mv);
// Smoothed cell voltage while on battery, 0 otherwise.
static inline int batgauge_cell_mv(const batgauge_t *g) { return g->v_q4 / 16; }
