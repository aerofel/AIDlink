// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "derive.h"
#include "geo.h"
#include <math.h>

static void seed_trk(derive_state_t *st, double trk_deg) {
    double r = trk_deg * M_PI / 180.0;
    st->trk_x = cos(r); st->trk_y = sin(r);
    st->trk_valid = true;
}

static void ema_trk(derive_state_t *st, double trk_deg) {
    if (!st->trk_valid) { seed_trk(st, trk_deg); return; }
    double r = trk_deg * M_PI / 180.0;
    st->trk_x += DERIVE_ALPHA * (cos(r) - st->trk_x);
    st->trk_y += DERIVE_ALPHA * (sin(r) - st->trk_y);
}

static double trk_out_deg(const derive_state_t *st) {
    double d = atan2(st->trk_y, st->trk_x) * 180.0 / M_PI;
    if (d < 0) d += 360.0;
    return d;
}

void derive_update(derive_state_t *st, double lat, double lon, uint32_t now_ms,
                   double gs_in, double trk_in, bool have_trk_in,
                   double *gs_out, double *trk_out, bool *have_trk_out) {
    // Source provides a value -> it is the truth; also seed the filters so a
    // later source dropout degrades gracefully instead of starting cold.
    if (gs_in >= 0) st->gs_f = gs_in;
    if (have_trk_in) seed_trk(st, trk_in);

    if (!st->have_prev) {
        st->prev_lat = lat; st->prev_lon = lon; st->prev_ms = now_ms;
        st->have_prev = true;
    } else if (gs_in < 0 || !have_trk_in) {
        double dnm  = geo_dist_nm(st->prev_lat, st->prev_lon, lat, lon);
        double dt_s = (now_ms - st->prev_ms) / 1000.0;
        if (dnm > DERIVE_MIN_NM && dt_s > 0.5) {
            // real movement since the last distinct position
            double inst = dnm / (dt_s / 3600.0);
            if (inst <= DERIVE_MAX_GS_KT) {
                if (gs_in < 0)
                    st->gs_f = (st->gs_f < 0) ? inst : st->gs_f + DERIVE_ALPHA * (inst - st->gs_f);
                if (!have_trk_in)
                    ema_trk(st, geo_bearing_deg(st->prev_lat, st->prev_lon, lat, lon));
            }
            // advance the baseline on movement (also past a rejected spike, so
            // one feed glitch doesn't poison the next sample)
            st->prev_lat = lat; st->prev_lon = lon; st->prev_ms = now_ms;
        } else if ((now_ms - st->prev_ms) > DERIVE_STILL_MS) {
            // provably (nearly) stationary for the whole window
            if (gs_in < 0) st->gs_f = 0;
            st->prev_ms = now_ms;   // rearm the window; keep position + heading
        }
        // position unchanged within the window: keep filtered values as-is
    } else {
        // source provides everything; just keep the baseline fresh
        st->prev_lat = lat; st->prev_lon = lon; st->prev_ms = now_ms;
    }

    *gs_out = (gs_in >= 0) ? gs_in : (st->gs_f >= 0 ? st->gs_f : 0);
    if (have_trk_in) { *trk_out = trk_in; *have_trk_out = true; }
    else if (st->trk_valid) { *trk_out = trk_out_deg(st); *have_trk_out = true; }
    else { *trk_out = 0; *have_trk_out = false; }
}
