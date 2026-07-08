// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Simulates the real Viasat feed behavior that broke naive derivation:
// 1 Hz polls, position updates only every 10 s, ~4-decimal quantization.
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "derive.h"
#include "geo.h"

static double q4(double v) { return round(v * 10000.0) / 10000.0; }

int main(void) {
    derive_state_t st = {0};
    double gs, trk; bool ht;

    // --- cruise: 470 kt, track 065, position served every 10th poll ---
    double lat = 0.0, lon = 30.0;
    double step_nm = 470.0 * 10.0 / 3600.0;                  // per 10 s
    double dlat = step_nm * cos(65.0 * M_PI / 180.0) / 60.0;
    double dlon = step_nm * sin(65.0 * M_PI / 180.0) / 60.0; // cos(lat)=1 at equator
    uint32_t t = 1000;
    int zeros_after_first_estimate = 0;
    bool have_estimate = false;
    for (int poll = 0; poll < 100; poll++) {
        if (poll && poll % 10 == 0) { lat += dlat; lon += dlon; }
        derive_update(&st, q4(lat), q4(lon), t, -1, 0, false, &gs, &trk, &ht);
        if (have_estimate && gs < 1) zeros_after_first_estimate++;
        if (gs > 1) have_estimate = true;
        t += 1000;
    }
    // never flaps back to 0 mid-cruise, converges near truth, stable heading
    assert(zeros_after_first_estimate == 0);
    assert(gs > 420 && gs < 520);
    assert(ht && fabs(trk - 65.0) < 4.0);

    // --- heading wrap: turn to track 358 crossing north ---
    for (int upd = 0; upd < 12; upd++) {
        double b = 358.0 * M_PI / 180.0;
        lat += step_nm * cos(b) / 60.0;
        lon += step_nm * sin(b) / 60.0;
        for (int i = 0; i < 10; i++) { derive_update(&st, q4(lat), q4(lon), t, -1, 0, false, &gs, &trk, &ht); t += 1000; }
    }
    assert(ht && (trk > 352.0 || trk < 4.0));   // never averages through 180

    // --- teleport spike rejected (feed glitch: 50 nm jump in one update) ---
    double gs_before = gs;
    lat += 50.0 / 60.0;
    derive_update(&st, q4(lat), q4(lon), t, -1, 0, false, &gs, &trk, &ht); t += 1000;
    assert(fabs(gs - gs_before) < 1.0);

    // --- stationary: same position for 40 s -> speed decays to 0 ---
    for (int i = 0; i < 40; i++) { derive_update(&st, q4(lat), q4(lon), t, -1, 0, false, &gs, &trk, &ht); t += 1000; }
    assert(gs < 1.0);
    assert(ht);                                  // heading is kept while parked

    // --- source-provided values pass through untouched (Panasonic) ---
    derive_update(&st, q4(lat), q4(lon), t, 333.0, 210.0, true, &gs, &trk, &ht);
    assert(gs == 333.0 && ht && trk == 210.0);

    printf("test_derive: PASS\n");
    return 0;
}
