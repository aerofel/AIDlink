// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Theoretical-profile arrival estimator (pure logic, host-tested).
//
// Why: the made-good estimator (eta.c) is honest but can only ever react —
// slow ground-speed oscillations and the not-yet-flown descent slowdown
// pass straight through it. A real FMS is steady because it predicts the
// ENTIRE remaining flight from a wind-corrected performance profile and only
// nudges that prediction with small measured deviations. This module ports
// the Offto app's profile model (see docs/superpowers/specs/
// 2026-07-15-theoretical-eta-design.md):
//
//   climb    3 segments, times from the aircraft DB, avg GS 280/380/M·593.7 kt
//   cruise   DB cruise TAS, split at 5°x60° climatology boxes, wind triangle
//            per segment (seasonal ERA5 250 hPa means), optional
//   descent  distance = ceiling/300 NM; time integrated over 40 steps of the
//            IAS schedule 290 → 250 @10000 ft AGL → 180 @4000 → 140 @0
//            (airport-elevation aware), IAS→TAS at ISA
//   approach last 60 NM staged 390/280/220/180/140 kt
//
// The ONLY adaptive term is a cruise bias: measured made-good speed vs the
// predicted cruise speed, EMA'd (τ 600 s), clamped ±10 %, and applied to the
// remaining cruise scaled by the fraction of cruise already flown — zero
// correction at top of climb, full (still slight) by end of cruise.
#pragma once
#include <stdbool.h>
#include "perfdb.h"

#define ETAP_MIN_GS_KT     80.0    // below this (taxi) no estimate
#define ETAP_APPROACH_NM   60.0    // staged-approach reserve (Offto parity)
#define ETAP_BIAS_TAU_S    600.0   // cruise bias EMA time constant
#define ETAP_BIAS_MIN      0.90    // bias ratio clamp — "very slight" by design
#define ETAP_BIAS_MAX      1.10
#define ETAP_OUT_TAU_S     60.0    // output EMA on the arrival/TOD epochs
#define ETAP_HYST_S        90.0    // displayed-minute hysteresis
#define ETAP_RESYNC_S      1800.0  // raw vs smoothed gap forcing a resync
#define ETAP_MAX_JUMP_KT   700.0   // teleport / destination-change guard
#define ETAP_JUMP_SLACK_NM 25.0    //   (same rationale + values as eta.c)

// cumulative (distance-from-origin, time) breakpoint table — ~1.3 KB, so it
// lives inside the caller-owned state, NOT on the calling task's stack (the
// display task runs on a small stack; a stack-local table panicked the S3)
#define ETAP_MAX_BP 80
typedef struct { double d[ETAP_MAX_BP], t[ETAP_MAX_BP]; int n; } etap_table_t;

typedef struct {
    double r_ema;                  // cruise bias ratio (1.0 = on-profile)
    bool   r_init;
    bool   have_eta, have_tod;
    double eta_s, tod_s;           // smoothed epochs, seconds
    long   shown_eta_min, shown_tod_min;
    double last_now, last_dist;    // jump/clock guards
    etap_table_t tb;               // per-update scratch (rebuilt every call)
} etap_state_t;

typedef struct {
    long eta_min;                  // displayed arrival epoch minutes, 0 = none
    long tod_min;                  // displayed TOD epoch minutes, 0 = none/passed
} etap_out_t;

void etap_reset(etap_state_t *st);

// Feed one sample. tot_nm = dep->arr great-circle, dist_to_go_nm = pos->arr
// (<0 = invalid fix), gs_made_good_kt from eta_made_good_kt() (<0 = not yet),
// month 1..12 selects the wind season, winds toggles the climatology.
// Returns displayed epochs in minutes; both 0 while no estimate is available.
etap_out_t etap_update(etap_state_t *st, const perf_ac_t *ac,
                       double lat, double lon,        // current position
                       double alat, double alon,      // arrival airport
                       int dest_elev_ft,
                       double tot_nm, double dist_to_go_nm,
                       double gs_inst_kt, double gs_made_good_kt,
                       double now_s, int month, bool winds);
