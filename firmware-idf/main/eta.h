// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Steady arrival-time estimator (pure logic, host-tested).
//
// The naive estimate now + dist/GS is hopeless for display: on a 4000 NM leg
// a ±10 kt GS wiggle swings it by ±10 minutes, and slow oscillations pass
// straight through any reasonable EMA. The steady ground speed is the
// *made-good* speed — distance covered over a 10-minute window — which
// integrates oscillations out exactly.
//
// But a windowed speed needs history, and the estimate should appear
// immediately. So the two are BLENDED by window coverage w = span/WIN:
// at first the instantaneous (derive.c) speed dominates — ETA shows within
// seconds, responsive but livelier — and as samples accumulate the made-good
// speed takes over. The EMA time constant and the display hysteresis ramp up
// along the same w, transitioning seamlessly into the fully steady behavior.
//
// Brief NCD blips blank the display but keep all state, so the estimate
// returns instantly when the feed recovers.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define ETA_MIN_GS_KT   80.0    // below this (taxi) no estimate
#define ETA_WIN_S       600.0   // made-good speed window (full steadiness)
#define ETA_MIN_SPAN_S  10.0    // history needed before made-good contributes
#define ETA_SAMPLE_S    5.0     // ring sample cadence
#define ETA_N           128     // ring capacity (>= WIN/SAMPLE + slack)
#define ETA_TAU_S       120.0   // EMA time constant on the arrival epoch (at w=1)
#define ETA_RESYNC_S    1800.0  // raw vs smoothed gap that forces a resync
#define ETA_SHOW_HYST_S 90.0    // displayed-minute hysteresis (at w=1)

// Destination-change detector: reset when the distance steps by more than a
// plausible closure rate PLUS a generous slack. The slack must tolerate fast
// bench replays (a x10-speed feed closes ~7 NM between 5 s samples) while any
// real destination change jumps by hundreds of NM and still trips it.
#define ETA_MAX_JUMP_KT   700.0
#define ETA_JUMP_SLACK_NM 25.0

typedef struct {
    int    count, head;               // sample ring (head = next write slot)
    double ts[ETA_N], ds[ETA_N];      // epoch s / distance-to-go NM
    bool   have_eta;
    double eta_s;                     // smoothed arrival epoch, seconds
    uint32_t mono_last;               // monotonic ms of the previous update
    bool   mono_have;                 //   (EMA dt source — NOT the epoch: the
                                      //   1 s epoch grain at a 2 Hz refresh
                                      //   integrated ~1.5 s per wall second)
    long   shown_min;                 // displayed arrival epoch, minutes
} eta_state_t;

void eta_reset(eta_state_t *st);

// Feed one sample: distance to destination (NM, <0 = unknown/invalid fix),
// instantaneous derived ground speed (kt, used while the window fills),
// current UTC epoch (s, 0 = unknown) and a monotonic millisecond clock
// (esp_timer on target; drives the filter dt at full resolution while the
// epoch anchors the displayed arrival time). Call at any cadence >= ~1 Hz;
// samples are decimated internally. Returns the arrival epoch to DISPLAY in
// minutes, or 0 when no estimate is available.
long eta_update(eta_state_t *st, double dist_nm, double gs_inst_kt,
                double now_s, uint32_t mono_ms);

// Window made-good ground speed (kt) straight from the sample ring, or -1
// while the ring spans under ETA_MIN_SPAN_S. This is the oscillation-proof
// measured speed the theoretical-profile estimator (eta_profile.c) compares
// against its predicted cruise speed.
double eta_made_good_kt(const eta_state_t *st);
