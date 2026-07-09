// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Steady arrival-time estimator — see eta.h for the design rationale.
#include "eta.h"
#include <math.h>

void eta_reset(eta_state_t *st) {
    st->count = 0; st->head = 0;
    st->have_eta = false; st->shown_min = 0; st->last_s = 0;
}

static int idx(const eta_state_t *st, int back) {   // back=0 -> newest sample
    return (st->head - 1 - back + 2 * ETA_N) % ETA_N;
}

long eta_update(eta_state_t *st, double dist_nm, double gs_inst_kt, double now_s) {
    if (dist_nm < 0 || now_s <= 0) {
        // invalid fix / no clock: blank the display but keep ALL state (ring,
        // smoothed epoch, shown minute). Clearing state here would make the
        // recovery reseed straight from the instantaneous raw value — at long
        // range a seconds-long position stall bends the made-good speed
        // enough to jump the raw ETA by whole minutes.
        return 0;
    }

    if (st->count) {
        double tn = st->ts[idx(st, 0)], dn = st->ds[idx(st, 0)];
        if (now_s < tn - 1.0) {
            eta_reset(st);                            // clock stepped backwards
        } else if (fabs(dist_nm - dn) > ETA_MAX_JUMP_KT / 3600.0 * (now_s - tn) + ETA_JUMP_SLACK_NM) {
            eta_reset(st);                            // destination change / teleport
        }
    }

    // decimated ring append + evict samples older than the window
    if (!st->count || now_s - st->ts[idx(st, 0)] >= ETA_SAMPLE_S) {
        if (st->count == ETA_N) st->count--;          // ring full: drop oldest
        st->ts[st->head] = now_s; st->ds[st->head] = dist_nm;
        st->head = (st->head + 1) % ETA_N;
        st->count++;
    }
    while (st->count > 2 && now_s - st->ts[idx(st, st->count - 1)] > ETA_WIN_S)
        st->count--;

    // Ground speed: blend instantaneous -> window made-good by coverage w.
    // The estimate is available immediately (from the derived instantaneous
    // speed) and hardens into the oscillation-proof made-good average.
    int oldest = idx(st, st->count - 1);
    double span = now_s - st->ts[oldest];
    double w = span / ETA_WIN_S;
    if (w > 1.0) w = 1.0;
    double gs;
    if (span >= ETA_MIN_SPAN_S) {
        double gs_win = (st->ds[oldest] - dist_nm) / span * 3600.0;
        // Blend only when the instantaneous speed is usable. derive.c reports
        // 0 when it can't trust the fixes (e.g. a x10 bench replay trips its
        // teleport rejection) — blending a bogus 0 would sink the result
        // below the floor and hide the ETA even though the window speed is
        // perfectly good.
        gs = (gs_inst_kt >= ETA_MIN_GS_KT) ? w * gs_win + (1.0 - w) * gs_inst_kt
                                           : gs_win;
    } else {
        gs = gs_inst_kt;
    }
    if (gs < ETA_MIN_GS_KT) { st->have_eta = false; return 0; }   // taxi/hold

    double raw = now_s + dist_nm / gs * 3600.0;
    double dt = st->last_s > 0 ? now_s - st->last_s : 0.5;
    st->last_s = now_s;
    if (!st->have_eta || fabs(raw - st->eta_s) > ETA_RESYNC_S) {
        st->have_eta = true;
        st->eta_s = raw;
        st->shown_min = lround(raw / 60.0);
    } else {
        // smoothing and display hysteresis stiffen with window coverage:
        // responsive while young, minute-steady once the window is full
        double tau  = ETA_TAU_S       * (0.1 + 0.9 * w);
        double hyst = ETA_SHOW_HYST_S * (0.2 + 0.8 * w);
        double a = 1.0 - exp(-(dt > 0 ? dt : 0.5) / tau);
        st->eta_s += (raw - st->eta_s) * a;
        if (fabs(st->eta_s - st->shown_min * 60.0) > hyst)
            st->shown_min = lround(st->eta_s / 60.0);
    }
    return st->shown_min;
}
