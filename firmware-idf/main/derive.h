// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Ground-speed / track derivation from successive fixes. Pure (host-testable).
//
// The real Viasat feed updates position only every ~10 s while we poll at 1 Hz,
// and serves ~11 m-quantized coordinates. Naive per-poll differencing therefore
// yields GS=0 between avionics updates and absurd spikes when the position
// finally jumps. This module:
//   - advances its baseline only on REAL movement (distinct positions),
//   - low-pass filters speed (EMA) and heading (vector EMA — wrap-safe),
//   - rejects teleport spikes, and
//   - decays speed to 0 only after proven stationarity.
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool     have_prev;
    double   prev_lat, prev_lon;   // last DISTINCT position (movement baseline)
    uint32_t prev_ms;
    double   gs_f;                 // filtered ground speed, kt (<0 = none yet)
    double   trk_x, trk_y;         // filtered heading unit-vector
    bool     trk_valid;
} derive_state_t;

#define DERIVE_MIN_NM       0.03   // movement gate (> position quantization)
#define DERIVE_ALPHA        0.35   // EMA weight for new samples
#define DERIVE_MAX_GS_KT    1200.0 // reject teleport samples above this
#define DERIVE_STILL_MS     30000  // no movement for this long -> speed 0

// Feed every accepted live fix. gs_in < 0 / have_trk_in false mean the source
// did not provide the value — derived+filtered values fill the gap.
// Outputs: *gs_out >= 0 always (0 until first estimate); *have_trk_out tells
// whether *trk_out is meaningful.
void derive_update(derive_state_t *st, double lat, double lon, uint32_t now_ms,
                   double gs_in, double trk_in, bool have_trk_in,
                   double *gs_out, double *trk_out, bool *have_trk_out);
