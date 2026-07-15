// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Theoretical-profile arrival estimator — see eta_profile.h for the design.
#include "eta_profile.h"
#include "geo.h"
#include <math.h>

#define D2R (M_PI / 180.0)
#define EARTH_NM 3440.065
#define MAX_BP 80                  // 4 climb + 2 cruise splits + 24 wind segs
                                   // + 40 descent steps + 5 approach stages

void etap_reset(etap_state_t *st) {
    st->r_ema = 1.0; st->r_init = false;
    st->have_eta = false; st->have_tod = false;
    st->eta_s = 0; st->tod_s = 0;
    st->shown_eta_min = 0; st->shown_tod_min = 0;
    st->last_now = 0; st->last_dist = 0;
}

// ISA IAS→TAS below the tropopause (Offto parity: TAS = IAS/sqrt(sigma),
// stratosphere temperature clamped)
static double ias_to_tas(double ias_kt, double alt_msl_ft) {
    double T = 288.15 - 0.0019812 * alt_msl_ft;
    if (T < 216.65) T = 216.65;
    return ias_kt * pow(288.15 / T, 2.128);
}

// wind triangle, meteorological convention (dir = FROM); Offto parity floors
static double wind_gs(double tas, double crs_deg, double wdir_deg, double wspd_kt) {
    if (tas <= 0) return 50.0;
    double d = (wdir_deg - crs_deg) * D2R;
    double swc = (wspd_kt / tas) * sin(d);
    if (fabs(swc) > 1.0) { double g = tas * 0.5; return g < 50.0 ? 50.0 : g; }
    double gs = tas * sqrt(1.0 - swc * swc) - wspd_kt * cos(d);
    return gs < 50.0 ? 50.0 : gs;
}

static void met_wind(double lat, double lon, int month, double *spd_kt, double *dir_deg) {
    double u, v;
    perfdb_wind(lat, lon, month, &u, &v);
    *spd_kt = hypot(u, v) * 1.944;
    double dir = atan2(-u, -v) / D2R;
    if (dir < 0) dir += 360.0;
    *dir_deg = dir;
}

// point at fraction f (0..1) along the great circle p1 -> p2
static void gc_slerp(double lat1, double lon1, double lat2, double lon2, double f,
                     double *lat, double *lon) {
    double d = geo_dist_nm(lat1, lon1, lat2, lon2) / EARTH_NM;   // central angle
    if (d < 1e-9) { *lat = lat2; *lon = lon2; return; }
    double p1 = lat1 * D2R, l1 = lon1 * D2R, p2 = lat2 * D2R, l2 = lon2 * D2R;
    double A = sin((1.0 - f) * d) / sin(d), B = sin(f * d) / sin(d);
    double x = A * cos(p1) * cos(l1) + B * cos(p2) * cos(l2);
    double y = A * cos(p1) * sin(l1) + B * cos(p2) * sin(l2);
    double z = A * sin(p1) + B * sin(p2);
    *lat = atan2(z, hypot(x, y)) / D2R;
    *lon = atan2(y, x) / D2R;
}

// cumulative (distance-from-origin, time) breakpoint table
typedef struct { double d[MAX_BP], t[MAX_BP]; int n; } table_t;

static void push(table_t *tb, double len_nm, double dur_s) {
    if (len_nm <= 0 || tb->n >= MAX_BP) return;
    tb->d[tb->n] = tb->d[tb->n - 1] + len_nm;
    tb->t[tb->n] = tb->t[tb->n - 1] + dur_s;
    tb->n++;
}

static double interp(const table_t *tb, double x) {
    if (x <= tb->d[0]) return tb->t[0];
    for (int i = 1; i < tb->n; i++)
        if (x <= tb->d[i]) {
            double span = tb->d[i] - tb->d[i - 1];
            double f = span > 1e-9 ? (x - tb->d[i - 1]) / span : 1.0;
            return tb->t[i - 1] + f * (tb->t[i] - tb->t[i - 1]);
        }
    return tb->t[tb->n - 1];
}

// EMA + minute hysteresis, shared by the arrival and TOD epochs
static long condition(double raw, double dt, double *sm, bool *have, long *shown) {
    if (!*have || fabs(raw - *sm) > ETAP_RESYNC_S) {
        *have = true;
        *sm = raw;
        *shown = lround(raw / 60.0);
    } else {
        double a = 1.0 - exp(-dt / ETAP_OUT_TAU_S);
        *sm += (raw - *sm) * a;
        if (fabs(*sm - *shown * 60.0) > ETAP_HYST_S)
            *shown = lround(*sm / 60.0);
    }
    return *shown;
}

etap_out_t etap_update(etap_state_t *st, const perf_ac_t *ac,
                       double lat, double lon, double alat, double alon,
                       int dest_elev_ft, double tot_nm, double dist_to_go_nm,
                       double gs_inst_kt, double gs_made_good_kt,
                       double now_s, int month, bool winds) {
    etap_out_t out = { 0, 0 };
    if (!ac || tot_nm <= 10.0 || now_s <= 0) return out;
    if (dist_to_go_nm < 0) return out;                 // NCD blip: keep state

    // clock-back / teleport guards (same rationale + values as eta.c)
    if (st->last_now > 0) {
        double gdt = now_s - st->last_now;
        if (now_s < st->last_now - 1.0 ||
            fabs(dist_to_go_nm - st->last_dist) >
                ETAP_MAX_JUMP_KT / 3600.0 * gdt + ETAP_JUMP_SLACK_NM)
            etap_reset(st);
    }
    double dt = st->last_now > 0 ? now_s - st->last_now : 0.5;
    if (dt <= 0) dt = 0.5;
    st->last_now = now_s;
    st->last_dist = dist_to_go_nm;

    double gs_ref = gs_made_good_kt > gs_inst_kt ? gs_made_good_kt : gs_inst_kt;
    if (gs_ref < ETAP_MIN_GS_KT) return out;           // taxi/hold: keep state

    // ---- theoretical profile geometry --------------------------------------
    double covered = tot_nm - dist_to_go_nm;
    if (covered < 0) covered = 0;                      // early off-track legs
    if (covered > tot_nm) covered = tot_nm;

    double tas = ac->cruise_kt;
    double d1 = ac->climb1_min / 60.0 * 280.0, t1 = ac->climb1_min * 60.0;
    double d2 = ac->climb2_min / 60.0 * 380.0, t2 = ac->climb2_min * 60.0;
    double d3 = ac->climb3_min / 60.0 * (ac->climb_mach * 593.7),
           t3 = ac->climb3_min * 60.0;
    double climb_d = d1 + d2 + d3;
    double desc_d = ac->ceiling_ft / 300.0;
    double cruise_d = tot_nm - climb_d - desc_d - ETAP_APPROACH_NM;
    if (cruise_d < 0) cruise_d = 0;                    // short leg: profile ends
    double tod_d = climb_d + cruise_d;                 //   past tot; floor below

    // ---- cruise bias: measured made-good vs predicted cruise speed ---------
    bool in_cruise = cruise_d > 20.0 && covered >= climb_d && covered < tod_d;
    double p = 0.0;                                    // fraction of cruise flown
    if (cruise_d > 20.0) {
        p = (covered - climb_d) / cruise_d;
        if (p < 0) p = 0;
        if (p > 1) p = 1;
    }
    if (in_cruise && gs_made_good_kt >= ETAP_MIN_GS_KT) {
        double gt = tas;                               // predicted GS right here
        if (winds) {
            double wspd, wdir;
            met_wind(lat, lon, month, &wspd, &wdir);
            gt = wind_gs(tas, geo_bearing_deg(lat, lon, alat, alon), wdir, wspd);
        }
        double r_raw = gs_made_good_kt / gt;
        if (!st->r_init) { st->r_ema = r_raw; st->r_init = true; }   // p≈0 anyway
        else {
            double a = 1.0 - exp(-dt / ETAP_BIAS_TAU_S);
            st->r_ema += (r_raw - st->r_ema) * a;
        }
        if (st->r_ema < ETAP_BIAS_MIN) st->r_ema = ETAP_BIAS_MIN;
        if (st->r_ema > ETAP_BIAS_MAX) st->r_ema = ETAP_BIAS_MAX;
    }
    double bias = 1.0 + (st->r_ema - 1.0) * p;         // scaled by cruise flown

    // ---- breakpoint table (dist-from-origin -> cumulative time) ------------
    table_t tb = { .d = { 0 }, .t = { 0 }, .n = 1 };
    push(&tb, d1, t1);
    push(&tb, d2, t2);
    push(&tb, d3, t3);

    // flown cruise at plain TAS (cancels out of every remaining-time
    // difference — only segments ahead of `covered` shape the output)
    double rem_from = covered > climb_d ? covered : climb_d;
    if (rem_from > tod_d) rem_from = tod_d;
    push(&tb, rem_from - climb_d, (rem_from - climb_d) / tas * 3600.0);

    // remaining cruise, wind-segmented along the pos->arr great circle
    double rem_cruise = tod_d - rem_from;
    if (rem_cruise > 0.5) {
        int nseg = (int)ceil(rem_cruise / 200.0);
        if (nseg < 1) nseg = 1;
        if (nseg > 24) nseg = 24;
        double seglen = rem_cruise / nseg;
        for (int i = 0; i < nseg; i++) {
            double gs = tas;
            if (winds && dist_to_go_nm > 1.0) {
                double along = rem_from + (i + 0.5) * seglen - covered;
                double f = along / dist_to_go_nm;
                if (f < 0) f = 0;
                if (f > 1) f = 1;
                double mlat, mlon, wspd, wdir;
                gc_slerp(lat, lon, alat, alon, f, &mlat, &mlon);
                met_wind(mlat, mlon, month, &wspd, &wdir);
                gs = wind_gs(tas, geo_bearing_deg(mlat, mlon, alat, alon), wdir, wspd);
            }
            gs *= bias;
            push(&tb, seglen, seglen / gs * 3600.0);
        }
    }

    // descent: 40-step integration of the IAS schedule, elevation-aware
    for (int i = 0; i < 40; i++) {
        double q = (i + 0.5) / 40.0;
        double alt = ac->ceiling_ft - q * (ac->ceiling_ft - dest_elev_ft);
        double agl = alt - dest_elev_ft;
        double ias = agl > 10000.0 ? 290.0
                   : agl > 4000.0  ? 180.0 + (agl - 4000.0) / 6000.0 * 70.0
                                   : 140.0 + agl / 4000.0 * 40.0;
        push(&tb, desc_d / 40.0, desc_d / 40.0 / ias_to_tas(ias, alt) * 3600.0);
    }

    // approach: staged speeds over the last 60 NM, single arrival wind
    double awspd = 0, awdir = 0, acrs = 0;
    if (winds) {
        double f60 = dist_to_go_nm > ETAP_APPROACH_NM
                   ? (dist_to_go_nm - ETAP_APPROACH_NM) / dist_to_go_nm : 0.0;
        double plat, plon;
        gc_slerp(lat, lon, alat, alon, f60, &plat, &plon);
        acrs = geo_bearing_deg(plat, plon, alat, alon);
        met_wind(alat, alon, month, &awspd, &awdir);
    }
    static const double ab[6] = { 60, 40, 25, 15, 8, 0 };
    static const double as[5] = { 390, 280, 220, 180, 140 };
    for (int i = 0; i < 5; i++) {
        double sp = winds ? wind_gs(as[i], acrs, awdir, awspd) : as[i];
        push(&tb, ab[i] - ab[i + 1], (ab[i] - ab[i + 1]) / sp * 3600.0);
    }

    // whole-profile floor (Offto parity: never faster than 0.8 x direct)
    double t_end = tb.t[tb.n - 1];
    double t_floor = 0.8 * tot_nm / tas * 3600.0;
    double scale = (t_end > 0 && t_end < t_floor) ? t_floor / t_end : 1.0;

    // ---- outputs ------------------------------------------------------------
    double at_cov = interp(&tb, covered);
    out.eta_min = condition(now_s + (t_end - at_cov) * scale, dt,
                            &st->eta_s, &st->have_eta, &st->shown_eta_min);
    if (covered < tod_d - 1.0 && cruise_d > 0.5) {
        out.tod_min = condition(now_s + (interp(&tb, tod_d) - at_cov) * scale, dt,
                                &st->tod_s, &st->have_tod, &st->shown_tod_min);
    } else {
        st->have_tod = false;                          // TOD passed: hide, stay 0
        st->shown_tod_min = 0;
    }
    return out;
}
