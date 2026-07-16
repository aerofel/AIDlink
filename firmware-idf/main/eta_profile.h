// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Theoretical-profile arrival estimator (pure logic, host-tested).
//
// Why: the made-good estimator (eta.c) is honest but can only ever react —
// slow ground-speed oscillations and the not-yet-flown descent slowdown
// pass straight through it. A real FMS is steady because it predicts the
// ENTIRE remaining flight from a wind-corrected performance profile and only
// nudges that prediction with small measured deviations. Model per
// docs/superpowers/specs/2026-07-16-orthodromic-vertical-eta-design.md
// (supersedes the 2026-07-15 flat-profile port):
//
//   geometry  orthodromic dep->arr, scaled by a frozen distance-only route
//             stretch (real routes fly 0-5.5 % over great circle; measured
//             on 12 real flights). Raw NM stays on the display.
//   vertical  initial cruise level from range fraction (semicircular-rule
//             ceiling by initial bearing), up to three 2000 ft step climbs
//             on equal-distance plateaus, DB upper-climb time scaled by
//             altitude with an ISA/Mach-capped climb speed
//   cruise    DB cruise TAS, wind triangle per 5°x60° climatology box
//   descent   3° proxy (final level / 300 NM), IAS schedule integrated to
//             60 NM out; the staged approach OVERLAYS the last 60 NM (the
//             old model descended to the field and then appended 60 NM,
//             putting TOD ~197 NM out vs the observed 99-162)
//   latch     live altitude latches a REAL early descent (proximity-gated
//             so a mid-cruise ATC descent can't wedge the profile)
//
// The ONLY adaptive term is a cruise bias: measured closure rate vs the
// predicted closure (path GS / stretch — closure and path speed differ by
// exactly the route stretch), EMA'd, clamped, and applied to the remaining
// cruise scaled by the fraction of cruise already flown. The p-scaling is
// LEVERAGE control, not just confidence (see the 2026-07-15 replay doc:
// confidence-ramp weighting was tested and rejected, span x2-3).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "perfdb.h"

#define ETAP_MIN_GS_KT     80.0    // below this (taxi) no estimate
#define ETAP_APPROACH_NM   60.0    // staged terminal segment (overlaid)
#ifndef ETAP_BIAS_TAU_S            // -D-overridable for replay experiments
#define ETAP_BIAS_TAU_S    2700.0  // cruise bias EMA: route-average, not gusts
#endif
// NOT APPLIED by default (r_ema still learns, for diagnostics): the matrix
// showed the applied bias buys 1.3 min of mean accuracy for 1.7x the churn
// (42 changes/2.5 reversals vs 25/0.9) — spec 6.2 says bias is optional and
// must not cost stability. Flip to 1 to re-engage the multiplier.
#ifndef ETAP_BIAS_APPLY
#define ETAP_BIAS_APPLY    0
#endif
#define ETAP_BIAS_MIN      0.90    // bias ratio clamp — "very slight" by design
#define ETAP_BIAS_MAX      1.10
#define ETAP_OUT_TAU_S     60.0    // output EMA on the arrival/TOD epochs
#define ETAP_HYST_S        90.0    // displayed-minute hysteresis at <=1 h to go
#define ETAP_HYST_SLOPE    60.0    //   +s per hour-to-go beyond 1 h: a minute
#define ETAP_HYST_CAP_S    420.0   //   flip 8 h out carries no information
#define ETAP_RESYNC_S      1800.0  // raw vs smoothed gap forcing a resync
#define ETAP_MAX_JUMP_KT   700.0   // teleport / destination-change guard
#define ETAP_JUMP_SLACK_NM 25.0    //   (same rationale + values as eta.c)

// Route stretch: pct = 5.5 * clamp((gc-1500)/3600, 0, 1)^4. Anchors: 1061 NM
// -> 0 %, 4395 NM -> 2.30 %, >=5100 NM -> 5.50 % (spec section 3.2).
// NOT APPLIED by default: the 2026-07-16 isolation matrix showed uniform
// stretch regresses the fleet span 22.9 -> 28.5 min because the DB's slow
// cruise TAS currently cancels the geometry error on the BKK/NOU family —
// enable together with per-route performance data, not alone. A front-
// loaded taper variant was also tested and also regressed (26.5-31.4 min).
#ifndef ETAP_STRETCH_APPLY
#define ETAP_STRETCH_APPLY   0
#endif
#define ETAP_STRETCH_MAX_PCT 5.5
#define ETAP_STRETCH_D0_NM   1500.0
#define ETAP_STRETCH_DW_NM   3600.0

// Early-descent latch (spec 5.2 + review note 2): all conditions must hold
// ETAP_LATCH_HOLD_S, and only within ETAP_LATCH_PROX x the nominal descent
// distance of the destination — a mid-cruise ATC descent never latches.
#define ETAP_LATCH_BELOW_FT  2000.0
#define ETAP_LATCH_VS_FPM    -300.0
#define ETAP_LATCH_HOLD_S    90.0
#define ETAP_LATCH_PROX      1.5

// cumulative (distance-from-origin, time) breakpoint table — lives inside the
// caller-owned state, NOT on the calling task's stack (the display task runs
// on a small stack; a stack-local table panicked the S3). Worst-case profile:
// 1 origin + 3 climb + 1 filler + 24 wind segments + 3 steps + 20 descent
// + 5 approach stages = 57; 96 leaves proven headroom (capacity host-tested).
#define ETAP_MAX_BP 96
typedef struct { double d[ETAP_MAX_BP], t[ETAP_MAX_BP]; int n; } etap_table_t;

#define ETAP_MAX_STEP 3

typedef struct {
    double r_ema;                  // cruise bias ratio (1.0 = on-profile)
    bool   r_init;
    bool   have_eta, have_tod;
    double eta_s, tod_s;           // smoothed epochs, seconds
    long   shown_eta_min, shown_tod_min;
    double last_now, last_dist;    // jump/clock guards (epoch-based)
    uint32_t mono_last;            // monotonic ms — the filter dt source
    bool   mono_have;

    // frozen route/vertical schedule (built once per origin/destination)
    bool   sch_ok;
    double sch_tot;                // raw GC total the schedule was built for
    double stretch;                // frozen factor (>= 1)
    double ptot;                   // stretched profile total NM
    int    nstep;                  // 0..ETAP_MAX_STEP
    double init_alt, final_alt;    // initial / final cruise level ft
    double climb_d, climb_t;       // to initial cruise (NM / s)
    double step_d[ETAP_MAX_STEP], step_t; // per-step NM / per-step s
    double step_at[ETAP_MAX_STEP]; // effective covered-NM where step k starts
    double desc_d;                 // final_alt/300 (last 60 NM overlaid)
    double tod_nm;                 // ptot - desc_d (effective space)

    // live-altitude descent latch
    double vs_fpm;                 // filtered vertical rate
    double alt_last;               // previous altitude sample
    uint32_t alt_last_ms;
    bool   alt_have;
    double latch_hold_s;           // condition-persistence accumulator
    bool   latched;
    double latch_cov, latch_alt;   // effective covered NM / ft at latch

    etap_table_t tb;               // per-update scratch (rebuilt every call)
} etap_state_t;

typedef struct {
    long eta_min;                  // displayed arrival epoch minutes, 0 = none
    long tod_min;                  // displayed TOD epoch minutes, 0 = none/passed
} etap_out_t;

void etap_reset(etap_state_t *st);

// Frozen route-stretch factor for a raw orthodromic total (pure; exposed for
// host tests and replay diagnostics). Returns >= 1.0.
double etap_stretch(double raw_total_gc_nm);

// Feed one sample.
//   olat/olon    departure airport (initial-bearing ceiling parity + schedule)
//   alat/alon    arrival airport
//   alt_ft       current altitude MSL (<0 = unknown; only ever LATCHES a real
//                early descent, never reshapes the schedule continuously)
//   tot_nm       dep->arr great-circle (raw; stretch is applied internally)
//   dist_to_go_nm  pos->arr great-circle (<0 = invalid fix)
//   gs_made_good_kt  from eta_made_good_kt() (<0 = not yet) — CLOSURE rate
//   now_s        UTC epoch seconds (anchors the displayed epochs)
//   mono_ms      monotonic milliseconds (drives every filter dt)
//   month 1..12 selects the wind season, winds toggles the climatology.
// Returns displayed epochs in minutes; both 0 while no estimate is available.
etap_out_t etap_update(etap_state_t *st, const perf_ac_t *ac,
                       double lat, double lon, double alt_ft,
                       double olat, double olon,
                       double alat, double alon,
                       int dest_elev_ft,
                       double tot_nm, double dist_to_go_nm,
                       double gs_inst_kt, double gs_made_good_kt,
                       double now_s, uint32_t mono_ms,
                       int month, bool winds);
