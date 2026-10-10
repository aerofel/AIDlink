// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "derive.h"
#include "geo.h"
#include <math.h>
#include <stddef.h>

#define M_PER_DEG 111319.5   // meters per degree of latitude (and lon at equator)

static void seed_trk(derive_state_t *st, double trk_deg) {
    double r = trk_deg * M_PI / 180.0;
    st->trk_x = cos(r); st->trk_y = sin(r);
    st->trk_valid = true;
}

// weight w in (0..1] scales the EMA step: noisy short-baseline samples pull
// the filtered heading proportionally less.
static void ema_trk(derive_state_t *st, double trk_deg, double w) {
    if (!st->trk_valid) { seed_trk(st, trk_deg); return; }
    double r = trk_deg * M_PI / 180.0, a = DERIVE_ALPHA * w;
    st->trk_x += a * (cos(r) - st->trk_x);
    st->trk_y += a * (sin(r) - st->trk_y);
}

static double trk_out_deg(const derive_state_t *st) {
    double d = atan2(st->trk_y, st->trk_x) * 180.0 / M_PI;
    if (d < 0) d += 360.0;
    return d;
}

static void push_fix(derive_state_t *st, double lat, double lon, uint32_t ms) {
    st->head = (st->head + 1) % DERIVE_RING_N;
    st->ring[st->head] = (derive_fix_t){ .lat = lat, .lon = lon, .ms = ms };
    if (st->count < DERIVE_RING_N) st->count++;
}

// Decimal places needed to reproduce v (0..DERIVE_MAX_DEC). A quantized value
// scaled by 10^d lands on an integer to well under the 1e-3 tolerance; an
// unquantized double walks the loop to the cap.
static int dec_places(double v) {
    double s = v;
    for (int d = 0; d < DERIVE_MAX_DEC; d++) {
        if (fabs(s - round(s)) < 1e-3) return d;
        s *= 10.0;
    }
    return DERIVE_MAX_DEC;
}

// Track the feed's per-axis precision as a sliding max: raise immediately,
// lower only when a whole 30-sample block stayed coarser — so values with
// trailing zeros ("171.310" looks 2-decimal) can't shrink the estimate.
static void upd_prec(derive_state_t *st, double lat, double lon) {
    int dla = dec_places(lat), dlo = dec_places(lon);
    if (dla > st->lat_dec) st->lat_dec = dla;
    if (dlo > st->lon_dec) st->lon_dec = dlo;
    if (dla > st->blk_lat) st->blk_lat = dla;
    if (dlo > st->blk_lon) st->blk_lon = dlo;
    if (++st->blk_n >= 30) {
        st->lat_dec = st->blk_lat; st->lon_dec = st->blk_lon;
        st->blk_lat = dla; st->blk_lon = dlo; st->blk_n = 0;
    }
}

static double pow10neg(int d) { double p = 1.0; while (d-- > 0) p /= 10.0; return p; }

// Worst-case bearing noise (deg) over a dnm baseline toward b_deg, from the
// per-axis quanta rotated into the cross-track direction: coarse longitude
// barely matters on an eastbound leg (it is along-track there) but dominates
// a northbound one. sqrt(2) covers both endpoints being off.
static double trk_err_deg(double b_deg, double dnm, double qlat_m, double qlon_m) {
    double D = dnm * 1852.0;
    if (D <= 0) return 1e9;
    double br = b_deg * M_PI / 180.0;
    double cx = cos(br) * qlon_m, cy = sin(br) * qlat_m;
    return M_SQRT2 * sqrt(cx * cx + cy * cy) / D * (180.0 / M_PI);
}

void derive_update(derive_state_t *st, double lat, double lon, uint32_t now_ms,
                   double gs_in, double trk_in, bool have_trk_in,
                   double *gs_out, double *trk_out, bool *have_trk_out) {
    // Source provides a value -> it is the truth; also seed the filters so a
    // later source dropout degrades gracefully instead of starting cold.
    if (gs_in >= 0) st->gs_f = gs_in;
    if (have_trk_in) seed_trk(st, trk_in);

    upd_prec(st, lat, lon);

    if (!st->count) {
        push_fix(st, lat, lon, now_ms);
    } else if (gs_in < 0 || !have_trk_in) {
        derive_fix_t *nw = &st->ring[st->head];
        double dnm  = geo_dist_nm(nw->lat, nw->lon, lat, lon);
        double dt_s = (now_ms - nw->ms) / 1000.0;
        if (dnm > DERIVE_MIN_NM && dt_s > 0.5) {
            if (dnm > DERIVE_MAX_GS_KT * (dt_s / 3600.0) + DERIVE_JUMP_SLACK_NM) {
                // teleport: restart the baseline here, keep the filtered
                // values, so one feed glitch doesn't poison the output
                st->count = 0;
                push_fix(st, lat, lon, now_ms);
            } else {
                double qlat_m = M_PER_DEG * pow10neg(st->lat_dec);
                double qlon_m = M_PER_DEG * cos(lat * M_PI / 180.0) * pow10neg(st->lon_dec);

                // trk ref: NEWEST ring entry whose baseline meets the bearing
                // error bound (shortest baseline that is quantization-clean);
                // gs ref: OLDEST entry inside the window (longest baseline —
                // time noise and quanta both vanish into it).
                const derive_fix_t *gs_ref = nw, *trk_ref = NULL;
                double trk_b = 0, trk_w = 0;
                // polyline distance to gs_ref through vertices >= DERIVE_GS_SEG_NM apart
                double path_nm = 0;
                double vlat = lat, vlon = lon;       // last committed vertex
                for (int i = 1; i < st->count; i++) {
                    const derive_fix_t *e =
                        &st->ring[(st->head - i + DERIVE_RING_N) % DERIVE_RING_N];
                    if (now_ms - e->ms > DERIVE_WIN_MS) break;
                    double seg = geo_dist_nm(vlat, vlon, e->lat, e->lon);
                    if (seg >= DERIVE_GS_SEG_NM) { path_nm += seg; vlat = e->lat; vlon = e->lon; }
                    gs_ref = e;
                    if (!trk_ref) {
                        double dn = geo_dist_nm(e->lat, e->lon, lat, lon);
                        if (dn > DERIVE_MIN_NM) {
                            double b = geo_bearing_deg(e->lat, e->lon, lat, lon);
                            if (trk_err_deg(b, dn, qlat_m, qlon_m) <= DERIVE_TRK_ERR_DEG) {
                                trk_ref = e; trk_b = b; trk_w = 1.0;
                            }
                        }
                    }
                }
                if (!trk_ref) {
                    // nothing clean enough yet (cold start / slow flight):
                    // best available baseline, de-weighted by its noise
                    double dn = geo_dist_nm(gs_ref->lat, gs_ref->lon, lat, lon);
                    if (dn > DERIVE_MIN_NM) {
                        trk_b = geo_bearing_deg(gs_ref->lat, gs_ref->lon, lat, lon);
                        double err = trk_err_deg(trk_b, dn, qlat_m, qlon_m);
                        trk_w = (err > DERIVE_TRK_ERR_DEG) ? DERIVE_TRK_ERR_DEG / err : 1.0;
                        trk_ref = gs_ref;
                    }
                }

                if (gs_in < 0) {
                    // polyline distance, not the chord: a chord across a turn
                    // under-measures the arc actually flown
                    double gdt_s = (now_ms - gs_ref->ms) / 1000.0;
                    if (gdt_s > 0.5) {
                        double gs_nm = path_nm + geo_dist_nm(vlat, vlon, gs_ref->lat, gs_ref->lon);
                        double inst = gs_nm / (gdt_s / 3600.0);
                        if (inst <= DERIVE_MAX_GS_KT)
                            st->gs_f = (st->gs_f < 0) ? inst
                                     : st->gs_f + DERIVE_ALPHA * (inst - st->gs_f);
                    }
                }
                if (!have_trk_in && trk_ref)
                    ema_trk(st, trk_b, trk_w);
                push_fix(st, lat, lon, now_ms);
            }
        } else if ((now_ms - nw->ms) > DERIVE_STILL_MS) {
            // provably (nearly) stationary for the whole window
            if (gs_in < 0) st->gs_f = 0;
            nw->ms = now_ms;   // rearm the window; keep position + heading
        }
        // position unchanged within the window: keep filtered values as-is
    } else {
        // source provides everything; just keep the baseline fresh
        st->count = 0;
        push_fix(st, lat, lon, now_ms);
    }

    *gs_out = (gs_in >= 0) ? gs_in : (st->gs_f >= 0 ? st->gs_f : 0);
    if (have_trk_in) { *trk_out = trk_in; *have_trk_out = true; }
    else if (st->trk_valid) { *trk_out = trk_out_deg(st); *have_trk_out = true; }
    else { *trk_out = 0; *have_trk_out = false; }
}

bool derive_align(derive_align_t *a, double lat, double lon, uint32_t now_ms,
                  double gs_kt, double trk_deg, bool have_trk,
                  double *out_lat, double *out_lon) {
    bool changed = !a->have || lat != a->lat || lon != a->lon;
    if (!a->have) { a->lat_ms = a->lon_ms = now_ms; }
    else {
        if (lat != a->lat) a->lat_ms = now_ms;
        if (lon != a->lon) a->lon_ms = now_ms;
    }
    a->have = true; a->lat = lat; a->lon = lon;
    *out_lat = lat; *out_lon = lon;
    if (!have_trk || !(gs_kt >= DERIVE_ALIGN_MIN_GS && gs_kt <= DERIVE_MAX_GS_KT)) return changed;

    // the newer axis sets the epoch; signed: the two stamps can straddle a wrap
    bool lat_newer = (int32_t)(a->lat_ms - a->lon_ms) >= 0;
    uint32_t lag = lat_newer ? a->lat_ms - a->lon_ms : a->lon_ms - a->lat_ms;
    if (lag == 0 || lag > DERIVE_ALIGN_MAX_MS) return changed;
    double nm = gs_kt * (lag / 3600000.0), r = trk_deg * M_PI / 180.0;
    if (lat_newer) {
        double c = cos(lat * M_PI / 180.0);
        if (c > 1e-6) {
            double o = lon + nm * sin(r) / (60.0 * c);
            if (o > 180.0) o -= 360.0; else if (o < -180.0) o += 360.0;
            *out_lon = o;
        }
    } else {
        double o = lat + nm * cos(r) / 60.0;
        if (o > 90.0) o = 90.0; else if (o < -90.0) o = -90.0;
        *out_lat = o;
    }
    return changed;
}
