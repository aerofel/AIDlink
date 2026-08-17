// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Ground-speed / track derivation from successive fixes. Pure (host-testable).
//
// Live feeds quantize their coordinates — and not evenly: an in-flight capture
// (2026-08-16) served latitude with 4 decimals (~11 m) but longitude with only
// 3 (~96 m at 30S). At jet speed the aircraft moves ~250 m/s, so differencing
// adjacent samples puts several degrees of quantization noise on the bearing
// (and the EFB ownship visibly twitches every second). Some feeds also repeat
// their position between ~10 s avionics updates while we poll at 1 Hz.
// This module therefore:
//   - estimates the feed's lat/lon decimal precision SEPARATELY (sliding-max,
//     so values with trailing zeros can't shrink the estimate),
//   - keeps a ring of past distinct positions and derives the bearing over the
//     shortest baseline whose projected error (per-axis quanta rotated into
//     the cross-track direction) stays under DERIVE_TRK_ERR_DEG — a northbound
//     leg with coarse longitude automatically stretches further back than an
//     eastbound one,
//   - derives ground speed over the oldest in-window position (a long time
//     baseline defeats both quantization and poll-timing granularity),
//   - low-pass filters speed (EMA) and heading (vector EMA — wrap-safe),
//     de-weighting heading samples when only a noisy baseline is available,
//   - rejects teleport spikes, and
//   - decays speed to 0 only after proven stationarity.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define DERIVE_RING_N       96     // past distinct fixes kept (96 s at 1 Hz)

typedef struct { double lat, lon; uint32_t ms; } derive_fix_t;

typedef struct {
    derive_fix_t ring[DERIVE_RING_N];  // distinct positions, ring[head] newest
    int      head, count;
    double   gs_f;                 // filtered ground speed, kt (<0 = none yet)
    double   trk_x, trk_y;         // filtered heading unit-vector
    bool     trk_valid;
    // per-axis quantization estimate: current decimals + sliding-max block
    int      lat_dec, lon_dec;
    int      blk_lat, blk_lon, blk_n;
} derive_state_t;

#define DERIVE_MIN_NM       0.03   // movement gate (> position quantization)
#define DERIVE_ALPHA        0.35   // EMA weight for new samples
#define DERIVE_MAX_GS_KT    1200.0 // reject teleport samples above this
#define DERIVE_STILL_MS     30000  // no movement for this long -> speed 0
#define DERIVE_WIN_MS       90000  // max reference age for track/GS baselines
#define DERIVE_TRK_ERR_DEG  0.7    // worst-case bearing noise accepted per sample
#define DERIVE_MAX_DEC      6      // finest decimal precision we try to detect

// Feed every accepted live fix. gs_in < 0 / have_trk_in false mean the source
// did not provide the value — derived+filtered values fill the gap.
// Outputs: *gs_out >= 0 always (0 until first estimate); *have_trk_out tells
// whether *trk_out is meaningful.
void derive_update(derive_state_t *st, double lat, double lon, uint32_t now_ms,
                   double gs_in, double trk_in, bool have_trk_in,
                   double *gs_out, double *trk_out, bool *have_trk_out);
