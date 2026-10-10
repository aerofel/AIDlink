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

    // --- staircase feed (in-flight capture 2026-10-10, ACI500 over Romania):
    // the cabin feed updates lat and lon at DIFFERENT instants (two avionics
    // words), so most polls move only one axis. Summed as a polyline the
    // staircase is up to |sin|+|cos| = 1.35x the distance flown on a 298 track:
    // the EFB was told 530-570 kt while ADS-B said 442.
    {
        derive_state_t s4 = {0};
        double crs = 298.0, kt = 442.0;
        double tla = 45.7, tlo = 23.9, fla = q4(tla), flo = q4(tlo);
        uint32_t tt = 1000;
        double max_gs_dev = 0;
        for (int i = 0; i < 300; i++) {
            double d = kt / 3600.0;
            tla += d * cos(crs * M_PI / 180.0) / 60.0;
            tlo += d * sin(crs * M_PI / 180.0) / (60.0 * cos(tla * M_PI / 180.0));
            if (i % 4 == 0) fla = q4(tla);           // lat word every 4 s
            if (i % 4 == 2) flo = q4(tlo);           // lon word every 4 s, 2 s later
            uint32_t jit = (uint32_t)((i * 7919) % 900);   // poll arrival jitter 0-0.9 s
            derive_update(&s4, fla, flo, tt + jit, -1, 0, false, &gs, &trk, &ht);
            if (i > 120 && fabs(gs - kt) > max_gs_dev) max_gs_dev = fabs(gs - kt);
            tt += 1000;
        }
        printf("  staircase: gs dev %.1f trk %.1f\n", max_gs_dev, trk);
        assert(max_gs_dev < 15.0);                   // ~3 %, was +88 kt
        assert(ht && fabs(trk - crs) < 2.0);
    }

    // --- derive_align: the staircase is removed from the PUBLISHED position.
    // Same feed as above (lat word every 4 s, lon word 2 s later); compare each
    // published fix with the true position at its stamp.
    {
        derive_align_t al = {0};
        double crs = 298.0, kt = 442.0, tla = 45.7, tlo = 23.9;
        double fla = q4(tla), flo = q4(tlo);
        double raw_max = 0, al_max = 0;
        for (int i = 0; i < 200; i++) {
            double d = kt / 3600.0;
            tla += d * cos(crs * M_PI / 180.0) / 60.0;
            tlo += d * sin(crs * M_PI / 180.0) / (60.0 * cos(tla * M_PI / 180.0));
            if (i % 4 == 0) fla = q4(tla);
            if (i % 4 == 2) flo = q4(tlo);
            double ola, olo;
            bool ch = derive_align(&al, fla, flo, 1000u * i, kt, crs, true, &ola, &olo);
            if (i > 8 && ch) {      // a fix is stamped when it changes
                double er = geo_dist_nm(fla, flo, tla, tlo), ea = geo_dist_nm(ola, olo, tla, tlo);
                if (er > raw_max) raw_max = er;
                if (ea > al_max) al_max = ea;
            }
        }
        printf("  align: raw err max %.3f NM, aligned %.3f NM\n", raw_max, al_max);
        assert(raw_max > 0.2);       // the staircase is real ...
        assert(al_max < 0.02);       // ... and gone after alignment (quantization only)
    }
    {
        derive_align_t al = {0}; double ola, olo;
        // first fix: changed, passes through
        assert(derive_align(&al, 10.0, 20.0, 1000, 450, 90, true, &ola, &olo) && ola == 10.0 && olo == 20.0);
        // repeat: not changed, still the same aligned fix
        assert(!derive_align(&al, 10.0, 20.0, 2000, 450, 90, true, &ola, &olo) && olo == 20.0);
        // both axes change together: no correction
        assert(derive_align(&al, 10.001, 20.001, 3000, 450, 45, true, &ola, &olo) && ola == 10.001 && olo == 20.001);
        // lat only, 2 s after lon: lon advanced east by 0.25 NM
        assert(derive_align(&al, 10.002, 20.001, 5000, 450, 90, true, &ola, &olo));
        assert(ola == 10.002 && fabs(geo_dist_nm(10.002, 20.001, ola, olo) - 0.25) < 0.005 && olo > 20.001);
        // slow: no correction
        assert(derive_align(&al, 10.003, 20.001, 7000, 30, 90, true, &ola, &olo) && olo == 20.001);
        // no track: no correction
        assert(derive_align(&al, 10.004, 20.001, 9000, 450, 90, false, &ola, &olo) && olo == 20.001);
        // lon unchanged for 20 s: it is not lagging (northbound), leave it
        assert(derive_align(&al, 10.005, 20.001, 25000, 450, 0, true, &ola, &olo) && olo == 20.001);
        // antimeridian: an eastbound lagging lon wraps
        derive_align_t w = {0};
        derive_align(&w, 0.0, 179.9999, 1000, 480, 90, true, &ola, &olo);
        derive_align(&w, 0.001, 179.9999, 5000, 480, 90, true, &ola, &olo);
        assert(olo < -179.9);
    }

    printf("test_derive: PASS\n");
    return 0;
}
