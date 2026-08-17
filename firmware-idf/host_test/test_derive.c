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

    // --- FOMAX-style feed: position updates EVERY poll (1 Hz), ~11 m quantized.
    // Per-second baselines are ~250 m at cruise, so endpoint rounding is worth
    // several degrees of bearing: derivation must pick a longer baseline, not
    // difference adjacent samples. Symptom this reproduces: ownship heading on
    // the EFB wandering a few degrees every second in steady cruise.
    {
        derive_state_t s2 = {0};
        double tla = -22.0, tlo = 166.0;             // true (unquantized) path
        double crs = 65.0, kt = 470.0;
        uint32_t tt = 1000;
        double prev_trk = 0, prev_gs = 0;
        double max_step = 0, max_dev = 0, max_gs_dev = 0, max_gs_step = 0;
        for (int i = 0; i < 150; i++) {
            double d = kt / 3600.0;                  // nm per second
            tla += d * cos(crs * M_PI / 180.0) / 60.0;
            tlo += d * sin(crs * M_PI / 180.0) / (60.0 * cos(tla * M_PI / 180.0));
            derive_update(&s2, q4(tla), q4(tlo), tt, -1, 0, false, &gs, &trk, &ht);
            if (i > 30) {                            // after convergence
                double dev = fabs(trk - crs);
                double stp = fabs(trk - prev_trk);
                if (dev > max_dev) max_dev = dev;
                if (stp > max_step) max_step = stp;
                double gdev = fabs(gs - kt), gstp = fabs(gs - prev_gs);
                if (gdev > max_gs_dev) max_gs_dev = gdev;
                if (gstp > max_gs_step) max_gs_step = gstp;
            }
            prev_trk = trk; prev_gs = gs;
            tt += 1000;
        }
        printf("  1Hz quantized: trk dev %.2f step %.2f | gs dev %.1f step %.1f\n",
               max_dev, max_step, max_gs_dev, max_gs_step);
        assert(ht);
        assert(max_dev  < 0.8);    // steady cruise: no visible heading wander
        assert(max_step < 0.35);   // and no per-second twitching on the EFB
        assert(max_gs_dev  < 8.0);
        assert(max_gs_step < 3.0);

        // turn guard: a longer baseline must not make heading unresponsive.
        // Standard 1.5 deg/s turn 065 -> 155, then hold: converge within 2 deg.
        for (int i = 0; i < 100; i++) {
            if (crs < 155.0) { crs += 1.5; if (crs > 155.0) crs = 155.0; }
            double d = kt / 3600.0;
            tla += d * cos(crs * M_PI / 180.0) / 60.0;
            tlo += d * sin(crs * M_PI / 180.0) / (60.0 * cos(tla * M_PI / 180.0));
            derive_update(&s2, q4(tla), q4(tlo), tt, -1, 0, false, &gs, &trk, &ht);
            tt += 1000;
        }
        printf("  after turn: trk %.2f gs %.1f\n", trk, gs);
        assert(ht && fabs(trk - 155.0) < 2.0);
    }

    // --- asymmetric precision (in-flight capture 2026-08-16): lat serves 4
    // decimals but lon only 3 -> ~96 m of east-west quantization at 30S, ~40 %
    // of the distance flown per second. A mostly-north course makes that error
    // nearly all cross-track: the baseline must stretch until the lon quantum
    // stops mattering, using the lat/lon quanta SEPARATELY.
    {
        derive_state_t s3 = {0};
        double tla = -30.4864, tlo = 171.306;
        double crs = 20.0, kt = 470.0;
        uint32_t tt = 1000;
        double prev_trk = 0, prev_gs = 0;
        double max_step = 0, max_dev = 0, max_gs_dev = 0, max_gs_step = 0;
        for (int i = 0; i < 240; i++) {
            double d = kt / 3600.0;
            tla += d * cos(crs * M_PI / 180.0) / 60.0;
            tlo += d * sin(crs * M_PI / 180.0) / (60.0 * cos(tla * M_PI / 180.0));
            double qla = round(tla * 10000.0) / 10000.0;   // 4-decimal lat
            double qlo = round(tlo * 1000.0) / 1000.0;     // 3-decimal lon
            derive_update(&s3, qla, qlo, tt, -1, 0, false, &gs, &trk, &ht);
            if (i > 80) {
                double dev = fabs(trk - crs), stp = fabs(trk - prev_trk);
                if (dev > max_dev) max_dev = dev;
                if (stp > max_step) max_step = stp;
                double gdev = fabs(gs - kt), gstp = fabs(gs - prev_gs);
                if (gdev > max_gs_dev) max_gs_dev = gdev;
                if (gstp > max_gs_step) max_gs_step = gstp;
            }
            prev_trk = trk; prev_gs = gs;
            tt += 1000;
        }
        printf("  q3 lon: trk dev %.2f step %.2f | gs dev %.1f step %.1f\n",
               max_dev, max_step, max_gs_dev, max_gs_step);
        assert(ht);
        assert(max_dev  < 1.2);
        assert(max_step < 0.5);
        assert(max_gs_dev  < 8.0);
        assert(max_gs_step < 3.0);
    }

    printf("test_derive: PASS\n");
    return 0;
}
