// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
// Host test for the steady ETA estimator (instantaneous -> made-good blend).
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "eta.h"

int main(void) {
    static eta_state_t st;
    eta_reset(&st);

    // --- no estimate without a distance / a clock / meaningful speed ---
    assert(eta_update(&st, -1, 480, 1000000, (uint32_t)((1000000) * 1000.0)) == 0);
    assert(eta_update(&st, 4000, 20, 1000000, (uint32_t)((1000000) * 1000.0)) == 0);         // taxiing
    assert(eta_update(&st, 4000, 480, 0, (uint32_t)((0) * 1000.0)) == 0);              // clock unknown

    // --- appears IMMEDIATELY from the instantaneous speed ---
    double now = 1000000, dist = 4000;
    long first = eta_update(&st, dist, 480, now + 1, (uint32_t)((now + 1) * 1000.0));
    assert(first == lround((now + 1 + dist / 480.0 * 3600.0) / 60.0));

    // --- cruise with ±10 kt slow oscillation: early estimate may move, but
    // once the window fills the DISPLAYED minute must hold still while the
    // naive minute keeps flapping.
    long shown = first, late_changes = 0, raw_changes = 0, raw_min = 0;
    for (int i = 1; i <= 2400; i++) {                        // 20 min of 0.5 s ticks
        double t = i * 0.5;
        double g = 480 + 10 * sin(t / 45.0);                 // ~4.7 min period
        dist -= g * 0.5 / 3600.0;
        long m = eta_update(&st, dist, g, now + t, (uint32_t)((now + t) * 1000.0));
        assert(m > 0);                                       // never blanks out
        if (m != shown) { shown = m; if (t > ETA_WIN_S) late_changes++; }
        long rm = lround((now + t + dist / g * 3600.0) / 60.0);
        if (raw_min && rm != raw_min) raw_changes++;
        raw_min = rm;
    }
    assert(raw_changes > 10);                                // naive display flaps
    assert(late_changes <= 2);                               // ours holds once mature
    assert(labs(shown - lround((now + 1200 + dist / 480.0 * 3600.0) / 60.0)) <= 2);

    // --- sustained slowdown 480 -> 430 kt: converges to the new truth ---
    double t0 = now + 1200;
    long m2 = 0;
    for (int i = 1; i <= 2400; i++) {                        // 20 min at 430 kt
        double t = i * 0.5;
        dist -= 430 * 0.5 / 3600.0;
        m2 = eta_update(&st, dist, 430, t0 + t, (uint32_t)((t0 + t) * 1000.0));
    }
    long truth = lround((t0 + 1200 + dist / 430.0 * 3600.0) / 60.0);
    assert(m2 > 0 && labs(m2 - truth) <= 3);

    // --- destination change: big dist jump resets, re-appears immediately ---
    dist += 2000;
    long m3 = eta_update(&st, dist, 430, t0 + 1201, (uint32_t)((t0 + 1201) * 1000.0));
    assert(m3 == lround(((t0 + 1201) + dist / 430.0 * 3600.0) / 60.0));

    // --- NCD blip blanks the display but keeps the history ---
    for (int i = 1; i <= 700; i++) {                         // rebuild some history
        double t = i * 0.5;
        dist -= 430 * 0.5 / 3600.0;
        m3 = eta_update(&st, dist, 430, t0 + 1201 + t, (uint32_t)((t0 + 1201 + t) * 1000.0));
    }
    assert(eta_update(&st, -1, 430, t0 + 1552, (uint32_t)((t0 + 1552) * 1000.0)) == 0);
    long m4 = eta_update(&st, dist, 430, t0 + 1553, (uint32_t)((t0 + 1553) * 1000.0));
    assert(m4 > 0 && labs(m4 - m3) <= 1);                    // instant recovery

    // --- x10-speed bench replay (~4800 kt) must not trip the teleport guard ---
    eta_reset(&st);
    double t2 = t0 + 4000, d2 = 2000;
    long m5 = 0;
    for (int i = 1; i <= 700; i++) {
        double t = i * 0.5;
        d2 -= 4800 * 0.5 / 3600.0;
        m5 = eta_update(&st, d2, 4800, t2 + t, (uint32_t)((t2 + t) * 1000.0));
        assert(m5 > 0);                                      // visible throughout
    }
    assert(labs(m5 - lround((t2 + 350 + d2 / 4800.0 * 3600.0) / 60.0)) <= 2);

    // --- gs_inst stuck at 0 (derive's teleport rejection on fast replays):
    // the window speed must carry the estimate alone, appearing within ~15 s.
    eta_reset(&st);
    double t3 = t2 + 4000, d3 = 2000;
    int appear_tick = 0;
    for (int i = 1; i <= 700; i++) {
        double t = i * 0.5;
        d3 -= 4800 * 0.5 / 3600.0;
        long m = eta_update(&st, d3, 0, t3 + t, (uint32_t)((t3 + t) * 1000.0));
        if (m > 0 && !appear_tick) appear_tick = i;
    }
    assert(appear_tick > 0 && appear_tick * 0.5 <= 16.0);    // shows within ~15 s
    assert(labs(eta_update(&st, d3, 0, t3 + 350.5, (uint32_t)((t3 + 350.5) * 1000.0))
                - lround((t3 + 350.5 + d3 / 4800.0 * 3600.0) / 60.0)) <= 2);

    printf("test_eta: PASS\n");
    return 0;
}
