// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Host test for the theoretical-profile ETA engine.
//   clang -Imain -o /tmp/t host_test/test_eta_profile.c main/eta_profile.c \
//         main/perfdb.c main/perfdb_data.c main/eta.c main/geo.c -lm && /tmp/t
//
// The simulator flies the SAME segment speeds as the engine's model (formulas
// duplicated here on purpose — they verify the implementation against the
// spec, not against itself), so the engine's prediction must both hold steady
// and land on the simulated arrival.
#include "eta_profile.h"
#include "eta.h"
#include "geo.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

// NWWW La Tontouta -> RJAA Narita (elev 141 ft)
#define DLAT (-22.0146)
#define DLON (166.2130)
#define ALAT (35.7647)
#define ALON (140.3864)
#define AELEV 141

static double t_ias_to_tas(double ias, double alt_ft) {
    double T = 288.15 - 0.0019812 * alt_ft;
    if (T < 216.65) T = 216.65;
    return ias * pow(288.15 / T, 2.128);
}

// spec-side model of the ground speed at a point along the profile (no wind)
static double sim_speed(const perf_ac_t *ac, double covered, double tot, int elev) {
    double d1 = ac->climb1_min / 60.0 * 280.0;
    double d2 = ac->climb2_min / 60.0 * 380.0;
    double d3 = ac->climb3_min / 60.0 * (ac->climb_mach * 593.7);
    double climb_d = d1 + d2 + d3;
    double desc_d = ac->ceiling_ft / 300.0;
    double cruise_d = tot - climb_d - desc_d - 60.0;
    double togo = tot - covered;
    if (togo <= 60.0) {                                  // staged approach
        return togo > 40 ? 390 : togo > 25 ? 280 : togo > 15 ? 220 : togo > 8 ? 180 : 140;
    }
    if (covered < d1) return 280;
    if (covered < d1 + d2) return 380;
    if (covered < climb_d) return ac->climb_mach * 593.7;
    if (covered < climb_d + cruise_d) return ac->cruise_kt;
    double q = (covered - climb_d - cruise_d) / desc_d;  // descent
    if (q > 1) q = 1;
    double alt = ac->ceiling_ft - q * (ac->ceiling_ft - elev);
    double agl = alt - elev;
    double ias = agl > 10000 ? 290.0
               : agl > 4000  ? 180.0 + (agl - 4000.0) / 6000.0 * 70.0
                             : 140.0 + agl / 4000.0 * 40.0;
    return t_ias_to_tas(ias, alt);
}

// spec-side expected total time (numeric integration over distance)
static double expected_total_s(const perf_ac_t *ac, double tot, int elev) {
    double t = 0, step = 0.05;
    for (double x = 0; x < tot; x += step)
        t += step / sim_speed(ac, x + step / 2, tot, elev) * 3600.0;
    return t;
}

static void slerp(double f, double *lat, double *lon) {   // dep->arr
    double d = geo_dist_nm(DLAT, DLON, ALAT, ALON) / 3440.065;
    double p1 = DLAT * M_PI / 180, l1 = DLON * M_PI / 180;
    double p2 = ALAT * M_PI / 180, l2 = ALON * M_PI / 180;
    double A = sin((1 - f) * d) / sin(d), B = sin(f * d) / sin(d);
    double x = A * cos(p1) * cos(l1) + B * cos(p2) * cos(l2);
    double y = A * cos(p1) * sin(l1) + B * cos(p2) * sin(l2);
    double z = A * sin(p1) + B * sin(p2);
    *lat = atan2(z, hypot(x, y)) * 180 / M_PI;
    *lon = atan2(y, x) * 180 / M_PI;
}

// full simulated flight; ratio scales the true speed vs the model (0.96 = a
// slow day), wiggle_kt adds a slow multiplicative oscillation in cruise
static void fly(const perf_ac_t *ac, bool winds, double ratio, double wiggle_amp,
                double *final_err_s, double *cruise_range_min,
                double *tod_range_min, int *tod_after_pass,
                double *err_q1, double *err_q3, double *r_after_20min) {
    double tot = geo_dist_nm(DLAT, DLON, ALAT, ALON);
    double t0 = 1767225600.0;                            // 2026-01-01 UTC
    eta_state_t ring; eta_reset(&ring);
    etap_state_t st; etap_reset(&st);

    double d1 = ac->climb1_min / 60.0 * 280.0, d2 = ac->climb2_min / 60.0 * 380.0;
    double climb_d = d1 + d2 + ac->climb3_min / 60.0 * (ac->climb_mach * 593.7);
    double desc_d = ac->ceiling_ft / 300.0;
    double cruise_d = tot - climb_d - desc_d - 60.0;
    double tod_d = climb_d + cruise_d;

    // truth arrival for error metrics
    double t_true = expected_total_s(ac, tot, AELEV) / ratio;

    double covered = 0, t = 0;
    long eta_min_lo = 0, eta_min_hi = 0, tod_lo = 0, tod_hi = 0;
    double first_show = -1;
    *tod_after_pass = 0; *r_after_20min = -1;
    *err_q1 = -1; *err_q3 = -1; *final_err_s = -1;
    double cruise_t_enter = -1;

    while (covered < tot - 0.3) {
        double wig = 1.0 + (wiggle_amp / 460.0) * sin(2 * M_PI * t / 1200.0);
        double gs_true = sim_speed(ac, covered, tot, AELEV) * ratio *
                         (covered > climb_d && covered < tod_d ? wig : 1.0);
        covered += gs_true * 5.0 / 3600.0;
        t += 5.0;
        double togo = tot - covered;
        if (togo < 0) togo = 0;
        double la, lo; slerp(covered / tot, &la, &lo);

        eta_update(&ring, togo, gs_true, t0 + t);
        etap_out_t o = etap_update(&st, ac, la, lo, ALAT, ALON, AELEV, tot, togo,
                                   gs_true, eta_made_good_kt(&ring),
                                   t0 + t, 1, winds);
        if (o.eta_min && first_show < 0) first_show = t;
        if (covered > climb_d && cruise_t_enter < 0) cruise_t_enter = t;
        // steadiness window: 15 min after entering cruise .. TOD
        if (cruise_t_enter > 0 && t > cruise_t_enter + 900 && covered < tod_d && o.eta_min) {
            if (!eta_min_lo || o.eta_min < eta_min_lo) eta_min_lo = o.eta_min;
            if (o.eta_min > eta_min_hi) eta_min_hi = o.eta_min;
        }
        if (cruise_t_enter > 0 && t > cruise_t_enter + 900 && covered < tod_d && o.tod_min) {
            if (!tod_lo || o.tod_min < tod_lo) tod_lo = o.tod_min;
            if (o.tod_min > tod_hi) tod_hi = o.tod_min;
        }
        if (covered > tod_d + 5 && o.tod_min) (*tod_after_pass)++;
        if (cruise_t_enter > 0 && *r_after_20min < 0 && t > cruise_t_enter + 1200)
            *r_after_20min = st.r_ema;
        double pc = cruise_d > 0 ? (covered - climb_d) / cruise_d : 0;
        if (*err_q1 < 0 && pc >= 0.25 && o.eta_min)
            *err_q1 = fabs(o.eta_min * 60.0 - (t0 + t_true));
        if (*err_q3 < 0 && pc >= 0.75 && o.eta_min)
            *err_q3 = fabs(o.eta_min * 60.0 - (t0 + t_true));
        if (togo < 30 && o.eta_min && *final_err_s < 0)
            *final_err_s = fabs(o.eta_min * 60.0 - (t0 + t_true));
    }
    assert(first_show > 0 && first_show <= 60);          // appears immediately
    *cruise_range_min = (double)(eta_min_hi - eta_min_lo);
    *tod_range_min = (double)(tod_hi - tod_lo);
}

int main(void) {
    const perf_ac_t *ac = perfdb_find("A339");
    assert(ac);
    double tot = geo_dist_nm(DLAT, DLON, ALAT, ALON);
    assert(tot > 3300 && tot < 4300);                    // sanity: NOU->NRT

    // --- IAS→TAS spot values (Offto formula) ---
    assert(fabs(t_ias_to_tas(250, 10000) - 291.0) < 3.0);
    assert(fabs(t_ias_to_tas(290, 39000) - 532.0) < 5.0);

    // --- first call = pure theoretical profile; must equal the spec model ---
    double t0 = 1767225600.0;
    etap_state_t st; etap_reset(&st);
    etap_out_t o = etap_update(&st, ac, DLAT, DLON, ALAT, ALON, AELEV, tot, tot,
                               460, -1, t0, 1, false);
    double t_exp = expected_total_s(ac, tot, AELEV);
    assert(o.eta_min > 0);
    assert(fabs((o.eta_min * 60.0 - t0) - t_exp) < 45.0);        // ±rounding
    assert(t_exp > 7.5 * 3600 && t_exp < 9.5 * 3600);            // plausible
    // TOD on first call: climb time + cruise time (no wind)
    double climb_d = ac->climb1_min / 60.0 * 280.0 + ac->climb2_min / 60.0 * 380.0
                   + ac->climb3_min / 60.0 * (ac->climb_mach * 593.7);
    double climb_t = (ac->climb1_min + ac->climb2_min + ac->climb3_min) * 60.0;
    double cruise_d = tot - climb_d - ac->ceiling_ft / 300.0 - 60.0;
    double tod_exp = climb_t + cruise_d / 460.0 * 3600.0;
    assert(o.tod_min > 0);
    assert(fabs((o.tod_min * 60.0 - t0) - tod_exp) < 45.0);
    assert(fabs(climb_d - 290.2) < 0.5);                 // A339 spec numbers
    assert(fabs(climb_t - 2430.0) < 0.1);
    assert(fabs(ac->ceiling_ft / 300.0 - 136.67) < 0.01);

    // --- gates: not flying / invalid / no aircraft ---
    etap_reset(&st);
    o = etap_update(&st, ac, DLAT, DLON, ALAT, ALON, AELEV, tot, tot, 20, -1, t0, 1, false);
    assert(o.eta_min == 0 && o.tod_min == 0);
    o = etap_update(&st, ac, DLAT, DLON, ALAT, ALON, AELEV, tot, -1, 460, -1, t0, 1, false);
    assert(o.eta_min == 0);
    o = etap_update(&st, 0, DLAT, DLON, ALAT, ALON, AELEV, tot, tot, 460, -1, t0, 1, false);
    assert(o.eta_min == 0);

    // --- steadiness: exact-model flight with ±25 kt slow cruise oscillation ---
    double ferr, crange, trange, eq1, eq3, r20; int tod_late;
    fly(ac, false, 1.0, 25.0, &ferr, &crange, &trange, &tod_late, &eq1, &eq3, &r20);
    printf("  steady: cruise ETA range %.0f min, TOD range %.0f min, final err %.0f s\n",
           crange, trange, ferr);
    assert(crange <= 3.0);                               // FMS-steady through cruise
    assert(trange <= 2.0);
    assert(tod_late == 0);                               // TOD hides once passed
    assert(ferr >= 0 && ferr <= 180.0);                  // lands on the sim arrival

    // --- bias: a 4 %-slow day; correction ramps with cruise fraction ---
    fly(ac, false, 0.96, 0.0, &ferr, &crange, &trange, &tod_late, &eq1, &eq3, &r20);
    printf("  bias:   r(+20min)=%.3f err@25%%=%.0fs err@75%%=%.0fs final=%.0fs\n",
           r20, eq1, eq3, ferr);
    assert(r20 > 0 && r20 < 0.99);                       // learned we're slow
    assert(eq3 <= eq1 + 30.0);                           // error shrinks with p
    assert(ferr >= 0 && ferr <= 300.0);

    // --- NCD blip keeps state; teleport resets ---
    eta_state_t ring; eta_reset(&ring);
    etap_reset(&st);
    double covered = tot / 2, t = 0;
    long before = 0;
    for (int i = 0; i < 400; i++) {                      // settle mid-cruise
        covered += 460.0 * 5 / 3600;
        t += 5;
        double la, lo; slerp(covered / tot, &la, &lo);
        eta_update(&ring, tot - covered, 460, t0 + t);
        o = etap_update(&st, ac, la, lo, ALAT, ALON, AELEV, tot, tot - covered,
                        460, eta_made_good_kt(&ring), t0 + t, 1, false);
    }
    before = o.eta_min;
    assert(before > 0);
    for (int i = 0; i < 12; i++) {                       // 60 s NCD blip
        covered += 460.0 * 5 / 3600;
        t += 5;
        o = etap_update(&st, ac, 0, 0, ALAT, ALON, AELEV, tot, -1, 460, -1, t0 + t, 1, false);
        assert(o.eta_min == 0 && o.tod_min == 0);
    }
    { double la, lo; slerp(covered / tot, &la, &lo);     // recovery: same minute
      covered += 460.0 * 5 / 3600; t += 5;
      eta_update(&ring, tot - covered, 460, t0 + t);
      o = etap_update(&st, ac, la, lo, ALAT, ALON, AELEV, tot, tot - covered,
                      460, eta_made_good_kt(&ring), t0 + t, 1, false); }
    assert(o.eta_min && labs(o.eta_min - before) <= 1);
    long pre_jump = o.eta_min;
    { double la, lo; slerp(0.2, &la, &lo);               // destination change
      t += 5;
      o = etap_update(&st, ac, la, lo, ALAT, ALON, AELEV, tot, (tot - covered) + 800,
                      460, -1, t0 + t, 1, false); }
    assert(o.eta_min > 0 && o.eta_min - pre_jump > 30);  // re-snapped, ~+1.7 h

    // --- winds: DJF Pacific jet is a net headwind northbound ---
    etap_state_t sa, sb, sc; etap_reset(&sa); etap_reset(&sb); etap_reset(&sc);
    etap_out_t on  = etap_update(&sa, ac, DLAT, DLON, ALAT, ALON, AELEV, tot, tot,
                                 460, -1, t0, 1, true);   // NOU->NRT winds ON
    etap_out_t off = etap_update(&sb, ac, DLAT, DLON, ALAT, ALON, AELEV, tot, tot,
                                 460, -1, t0, 1, false);
    etap_out_t rev = etap_update(&sc, ac, ALAT, ALON, DLAT, DLON, 52, tot, tot,
                                 460, -1, t0, 1, true);   // NRT->NOU winds ON
    assert(on.eta_min > off.eta_min + 5);                 // slower into the jet
    assert(on.eta_min > rev.eta_min + 10);                // asymmetry, same track
    printf("  winds:  NOU->NRT %+ld min vs still air; asymmetry %ld min\n",
           on.eta_min - off.eta_min, on.eta_min - rev.eta_min);

    // --- made-good accessor ---
    eta_reset(&ring);
    assert(eta_made_good_kt(&ring) == -1);
    for (int i = 0; i <= 180; i++)
        eta_update(&ring, 3000 - 480.0 * i * 5 / 3600, 480, t0 + i * 5);
    assert(fabs(eta_made_good_kt(&ring) - 480.0) < 5.0);

    printf("test_eta_profile: PASS\n");
    return 0;
}
