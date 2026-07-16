// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Host test for the theoretical-profile ETA engine (orthodromic + vertical
// model, docs/superpowers/specs/2026-07-16-orthodromic-vertical-eta-design.md).
//   clang -Imain -o /tmp/t host_test/test_eta_profile.c main/eta_profile.c \
//         main/perfdb.c main/perfdb_data.c main/eta.c main/geo.c -lm && /tmp/t
//
// The simulator flies the SAME schedule/speeds as the engine's model
// (formulas duplicated here on purpose — they verify the implementation
// against the spec, not against itself), so the engine's prediction must
// both hold steady and land on the simulated arrival.
#include "eta_profile.h"
#include "eta.h"
#include "geo.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

// NWWW La Tontouta -> RJAA Narita (elev 141 ft) — the main simulated flight
#define DLAT (-22.0146)
#define DLON (166.2130)
#define ALAT (35.7647)
#define ALON (140.3864)
#define AELEV 141

// anchor airports (spec section 4.3)
#define CDG_LAT 49.0097
#define CDG_LON 2.5479
#define BKK_LAT 13.6900
#define BKK_LON 100.7501
#define NOU_LAT (-22.0146)
#define NOU_LON 166.2130
#define SYD_LAT (-33.9461)
#define SYD_LON 151.1772

// ---- spec-side formula duplicates ------------------------------------------
static double t_ias_to_tas(double ias, double alt_ft) {
    double T = 288.15 - 0.0019812 * alt_ft;
    if (T < 216.65) T = 216.65;
    return ias * pow(288.15 / T, 2.128);
}
static double t_isa_a(double alt_ft) {
    double T = 288.15 - 0.0019812 * alt_ft;
    if (T < 216.65) T = 216.65;
    return 661.47 * sqrt(T / 288.15);
}
static double t_climb_tas(const perf_ac_t *ac, double alt_ft) {
    double a = t_ias_to_tas(300.0, alt_ft), b = ac->climb_mach * t_isa_a(alt_ft);
    return a < b ? a : b;
}
static double t_stretch(double gc) {
    double x = (gc - 1500.0) / 3600.0;
    if (x < 0) x = 0;
    if (x > 1) x = 1;
    return 1.0 + 5.5 * x * x * x * x / 100.0;
}

// spec-side frozen schedule
typedef struct {
    double stretch, ptot;
    int nstep;
    double init_alt, final_alt;
    double climb_d, climb_t, step_d[3], step_t, step_at[3];
    double desc_d, tod;
} sched_t;

static void t_sched(const perf_ac_t *ac, double olat, double olon,
                    double alat, double alon, sched_t *s) {
    double tot = geo_dist_nm(olat, olon, alat, alon);
    // engine default: ETAP_STRETCH_APPLY 0 (matrix-regressed until per-route
    // performance data lands); the curve itself is anchored via etap_stretch()
    s->stretch = ETAP_STRETCH_APPLY ? t_stretch(tot) : 1.0;
    s->ptot = tot * s->stretch;
    bool east = geo_bearing_deg(olat, olon, alat, alon) < 180.0;
    int fl = (int)(ac->ceiling_ft / 1000);
    if (((fl & 1) == 1) != east) fl -= 1;
    double usable = fl * 1000.0;
    double rf = s->ptot / ac->max_range_nm;
    if (rf > 1) rf = 1;
    s->nstep = (int)lround(3.0 * rf * rf);
    s->init_alt = usable - 2000.0 * s->nstep;
    s->final_alt = s->init_alt + 2000.0 * s->nstep;
    double span = ac->ceiling_ft - 20000.0;
    double t3 = ac->climb3_min * 60.0 * (s->init_alt - 20000.0) / span;
    s->climb_d = ac->climb1_min / 60.0 * 280.0 + ac->climb2_min / 60.0 * 380.0
               + t3 / 3600.0 * t_climb_tas(ac, (20000.0 + s->init_alt) / 2.0);
    s->climb_t = (ac->climb1_min + ac->climb2_min) * 60.0 + t3;
    s->step_t = ac->climb3_min * 60.0 * 2000.0 / span;
    double sd = 0;
    for (int k = 0; k < s->nstep; k++) {
        s->step_d[k] = s->step_t / 3600.0 * t_climb_tas(ac, s->init_alt + 2000.0 * k + 1000.0);
        sd += s->step_d[k];
    }
    s->desc_d = s->final_alt / 300.0;
    double level = s->ptot - s->climb_d - sd - s->desc_d;
    double plat = level / (s->nstep + 1);
    double at = s->climb_d;
    for (int k = 0; k < s->nstep; k++) { at += plat; s->step_at[k] = at; at += s->step_d[k]; }
    s->tod = s->climb_d + level + sd;
}

// spec-side profile speed at effective covered distance x (no wind)
static double sim_speed(const perf_ac_t *ac, const sched_t *s, double x, int elev) {
    double togo = s->ptot - x;
    if (togo <= 60.0)                                    // staged approach
        return togo > 40 ? 390 : togo > 25 ? 280 : togo > 15 ? 220 : togo > 8 ? 180 : 140;
    double d1 = ac->climb1_min / 60.0 * 280.0, d2 = ac->climb2_min / 60.0 * 380.0;
    if (x < d1) return 280;
    if (x < d1 + d2) return 380;
    if (x < s->climb_d) return t_climb_tas(ac, (20000.0 + s->init_alt) / 2.0);
    if (x >= s->tod) {                                   // upper descent
        double dud = s->desc_d - 60.0;
        double q = (x - s->tod) / dud;
        if (q > 1) q = 1;
        double alt = s->final_alt - q * (s->final_alt - (elev + 18000.0));
        double agl = alt - elev;
        double ias = agl > 10000.0 ? 290.0
                   : agl > 4000.0  ? 180.0 + (agl - 4000.0) / 6000.0 * 70.0
                                   : 140.0 + agl / 4000.0 * 40.0;
        return t_ias_to_tas(ias, alt);
    }
    for (int k = 0; k < s->nstep; k++)                   // step climbs
        if (x >= s->step_at[k] && x < s->step_at[k] + s->step_d[k])
            return t_climb_tas(ac, s->init_alt + 2000.0 * k + 1000.0);
    return ac->cruise_kt;
}

// spec-side scheduled altitude at effective covered distance x
static double sim_alt(const sched_t *s, double x, int elev) {
    if (x < s->climb_d) return 1000.0 + (s->init_alt - 1000.0) * x / s->climb_d;
    if (x >= s->tod) {
        double q = (x - s->tod) / s->desc_d;
        if (q > 1) q = 1;
        return s->final_alt - q * (s->final_alt - elev);
    }
    int plat = 0;
    for (int k = 0; k < s->nstep; k++)
        if (x >= s->step_at[k] + s->step_d[k]) plat = k + 1;
        else if (x >= s->step_at[k])
            return s->init_alt + 2000.0 * k + 2000.0 * (x - s->step_at[k]) / s->step_d[k];
    return s->init_alt + 2000.0 * plat;
}

// spec-side expected total time (numeric integration over distance)
static double expected_total_s(const perf_ac_t *ac, const sched_t *s, int elev) {
    double t = 0, step = 0.05;
    for (double x = 0; x < s->ptot; x += step)
        t += step / sim_speed(ac, s, x + step / 2, elev) * 3600.0;
    return t;
}

static void slerp(double f, double *lat, double *lon) {   // dep->arr NOU->NRT
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

static long g_max_step;          // largest displayed-ETA jump seen in a fly()

// full simulated flight in EFFECTIVE space: the aircraft flies the schedule's
// path speed; the engine sees the RAW closure (path / stretch) — exactly the
// geometry a stretched real route produces. ratio scales true vs model speed
// (0.96 = a slow day), wiggle_amp adds a slow oscillation in cruise.
static void fly(const perf_ac_t *ac, bool winds, double ratio, double wiggle_amp,
                double *final_err_s, double *cruise_range_min,
                double *tod_range_min, int *tod_after_pass,
                double *err_q1, double *err_q3, double *r_late) {
    sched_t sc; t_sched(ac, DLAT, DLON, ALAT, ALON, &sc);
    double tot = geo_dist_nm(DLAT, DLON, ALAT, ALON);
    double t0 = 1767225600.0;                            // 2026-01-01 UTC
    eta_state_t ring; eta_reset(&ring);
    etap_state_t st; etap_reset(&st);

    double t_true = expected_total_s(ac, &sc, AELEV) / ratio;

    double cov = 0, t = 0;                               // effective covered
    long eta_lo = 0, eta_hi = 0, tod_lo = 0, tod_hi = 0, eta_prev = 0;
    double first_show = -1;
    *tod_after_pass = 0; *r_late = -1;
    *err_q1 = -1; *err_q3 = -1; *final_err_s = -1;
    double cruise_t_enter = -1;
    g_max_step = 0;

    while (cov < sc.ptot - 0.3) {
        double wig = 1.0 + (wiggle_amp / 460.0) * sin(2 * M_PI * t / 1200.0);
        bool lvl = cov > sc.climb_d && cov < sc.tod;
        double v = sim_speed(ac, &sc, cov, AELEV) * ratio * (lvl ? wig : 1.0);
        cov += v * 5.0 / 3600.0;
        t += 5.0;
        double togo_raw = (sc.ptot - cov) / sc.stretch;
        if (togo_raw < 0) togo_raw = 0;
        double la, lo; slerp(1.0 - togo_raw / tot, &la, &lo);
        double alt = sim_alt(&sc, cov, AELEV);

        eta_update(&ring, togo_raw, v / sc.stretch, t0 + t, (uint32_t)(t * 1000));
        etap_out_t o = etap_update(&st, ac, la, lo, alt, DLAT, DLON, ALAT, ALON,
                                   AELEV, tot, togo_raw,
                                   v / sc.stretch, eta_made_good_kt(&ring),
                                   t0 + t, (uint32_t)(t * 1000), 1, winds);
        if (o.eta_min && first_show < 0) first_show = t;
        if (o.eta_min && eta_prev && labs(o.eta_min - eta_prev) > g_max_step)
            g_max_step = labs(o.eta_min - eta_prev);
        if (o.eta_min) eta_prev = o.eta_min;
        if (cov > sc.climb_d && cruise_t_enter < 0) cruise_t_enter = t;
        // steadiness window: 15 min after entering cruise .. TOD
        if (cruise_t_enter > 0 && t > cruise_t_enter + 900 && cov < sc.tod && o.eta_min) {
            if (!eta_lo || o.eta_min < eta_lo) eta_lo = o.eta_min;
            if (o.eta_min > eta_hi) eta_hi = o.eta_min;
        }
        if (cruise_t_enter > 0 && t > cruise_t_enter + 900 && cov < sc.tod && o.tod_min) {
            if (!tod_lo || o.tod_min < tod_lo) tod_lo = o.tod_min;
            if (o.tod_min > tod_hi) tod_hi = o.tod_min;
        }
        if (cov > sc.tod + 5 && o.tod_min) (*tod_after_pass)++;
        // τ 2700 s (true, monotonic-clocked): sample the bias 40 min into cruise
        if (cruise_t_enter > 0 && *r_late < 0 && t > cruise_t_enter + 2400)
            *r_late = st.r_ema;
        double pc = (cov - sc.climb_d) / (sc.tod - sc.climb_d);
        if (*err_q1 < 0 && pc >= 0.25 && o.eta_min)
            *err_q1 = fabs(o.eta_min * 60.0 - (t0 + t_true));
        if (*err_q3 < 0 && pc >= 0.75 && o.eta_min)
            *err_q3 = fabs(o.eta_min * 60.0 - (t0 + t_true));
        if (togo_raw < 30 && o.eta_min && *final_err_s < 0)
            *final_err_s = fabs(o.eta_min * 60.0 - (t0 + t_true));
    }
    assert(first_show > 0 && first_show <= 60);          // appears immediately
    *cruise_range_min = (double)(eta_hi - eta_lo);
    *tod_range_min = (double)(tod_hi - tod_lo);
}

// first-call schedule probe: one valid update, then read the frozen schedule
static void probe(const perf_ac_t *ac, double olat, double olon,
                  double alat, double alon, etap_state_t *st) {
    etap_reset(st);
    double tot = geo_dist_nm(olat, olon, alat, alon);
    etap_update(st, ac, olat, olon, -1, olat, olon, alat, alon, 20,
                tot, tot, 460, -1, 1767225600.0, 1000, 1, false);
    assert(st->sch_ok);
}

int main(void) {
    const perf_ac_t *ac = perfdb_find("A339");
    assert(ac && ac->max_range_nm == 5500);
    double tot = geo_dist_nm(DLAT, DLON, ALAT, ALON);
    assert(tot > 3300 && tot < 4300);                    // sanity: NOU->NRT

    // --- IAS→TAS / ISA spot values ---
    assert(fabs(t_ias_to_tas(250, 10000) - 291.0) < 3.0);
    assert(fabs(t_ias_to_tas(290, 39000) - 532.0) < 5.0);
    assert(fabs(t_isa_a(0) - 661.47) < 0.1);

    // --- route stretch anchors (spec 3.2) ---
    assert(fabs(etap_stretch(1061.0) - 1.0) < 1e-9);
    assert(fabs(etap_stretch(4395.0) - 1.0230) < 3e-4);
    assert(fabs(etap_stretch(5101.0) - 1.0550) < 3e-4);
    assert(fabs(etap_stretch(9000.0) - 1.0550) < 1e-9);  // capped
    for (double d = 0; d < 8000; d += 50)                // monotonic, bounded
        assert(etap_stretch(d + 50) >= etap_stretch(d) &&
               etap_stretch(d) >= 1.0 && etap_stretch(d) <= 1.055 + 1e-12);

    // --- vertical anchors (spec 4.3): ceiling parity + initial level -------
    etap_state_t st;
    probe(ac, SYD_LAT, SYD_LON, NOU_LAT, NOU_LON, &st);  // 1061 NM eastbound
    assert(st.nstep == 0 && st.init_alt == 41000 && st.final_alt == 41000);
    probe(ac, BKK_LAT, BKK_LON, NOU_LAT, NOU_LON, &st);  // 4395 NM eastbound
    assert(st.nstep == 2 && st.init_alt == 37000 && st.final_alt == 41000);
    probe(ac, NOU_LAT, NOU_LON, BKK_LAT, BKK_LON, &st);  // westbound: even
    assert(st.nstep == 2 && st.init_alt == 36000 && st.final_alt == 40000);
    probe(ac, CDG_LAT, CDG_LON, BKK_LAT, BKK_LON, &st);  // 5101 NM eastbound
    assert(st.nstep == 3 && st.init_alt == 35000 && st.final_alt == 41000);
    probe(ac, BKK_LAT, BKK_LON, CDG_LAT, CDG_LON, &st);  // westbound: even
    assert(st.nstep == 3 && st.init_alt == 34000 && st.final_alt == 40000);

    // upper-climb scaling: ~2.38 min per 2000 ft step for the A339,
    // FL200->FL350 in ~17.86 min (25 min * 15000/21000)
    assert(fabs(st.step_t - 2.381 * 60.0) < 1.0);
    assert(fabs(st.climb_t - (6.5 + 9.0) * 60.0 - 25.0 * 60.0 * 14000.0 / 21000.0) < 2.0);

    // descent overlay: TOD sits desc_d (not desc_d + 60) from the end, and
    // the table spans exactly the stretched total (no truncation, MAX_BP ok)
    assert(fabs((st.ptot - st.tod_nm) - st.final_alt / 300.0) < 0.1);
    assert(st.tb.n < ETAP_MAX_BP);
    assert(fabs(st.tb.d[st.tb.n - 1] - st.ptot) < 0.5);

    // --- first call = pure theoretical profile; must equal the spec model ---
    double t0 = 1767225600.0;
    sched_t sc; t_sched(ac, DLAT, DLON, ALAT, ALON, &sc);
    etap_reset(&st);
    etap_out_t o = etap_update(&st, ac, DLAT, DLON, -1, DLAT, DLON, ALAT, ALON,
                               AELEV, tot, tot, 460, -1, t0, 1000, 1, false);
    double t_exp = expected_total_s(ac, &sc, AELEV);
    assert(o.eta_min > 0);
    assert(fabs((o.eta_min * 60.0 - t0) - t_exp) < 45.0);        // ±rounding
    assert(t_exp > 7.5 * 3600 && t_exp < 9.5 * 3600);            // plausible
    assert(o.tod_min > 0);
    double tod_exp = 0;                                  // spec: integrate to TOD
    for (double x = 0; x < sc.tod; x += 0.05)
        tod_exp += 0.05 / sim_speed(ac, &sc, x + 0.025, AELEV) * 3600.0;
    assert(fabs((o.tod_min * 60.0 - t0) - tod_exp) < 60.0);

    // --- gates: not flying / invalid / no aircraft ---
    etap_reset(&st);
    o = etap_update(&st, ac, DLAT, DLON, -1, DLAT, DLON, ALAT, ALON, AELEV,
                    tot, tot, 20, -1, t0, 1000, 1, false);
    assert(o.eta_min == 0 && o.tod_min == 0);
    o = etap_update(&st, ac, DLAT, DLON, -1, DLAT, DLON, ALAT, ALON, AELEV,
                    tot, -1, 460, -1, t0, 1500, 1, false);
    assert(o.eta_min == 0);
    o = etap_update(&st, 0, DLAT, DLON, -1, DLAT, DLON, ALAT, ALON, AELEV,
                    tot, tot, 460, -1, t0, 2000, 1, false);
    assert(o.eta_min == 0);

    // --- steadiness: exact-model flight with ±25 kt slow cruise oscillation ---
    double ferr, crange, trange, eq1, eq3, r40; int tod_late;
    fly(ac, false, 1.0, 25.0, &ferr, &crange, &trange, &tod_late, &eq1, &eq3, &r40);
    printf("  steady: cruise ETA range %.0f min, TOD range %.0f min, final err %.0f s, max_step %ld\n",
           crange, trange, ferr, g_max_step);
    assert(crange <= 3.0);                               // FMS-steady through cruise
    assert(trange <= 3.0);                               // (includes step epochs)
    assert(tod_late == 0);                               // TOD hides once passed
    assert(ferr >= 0 && ferr <= 180.0);                  // lands on the sim arrival
    assert(g_max_step <= 1);                             // 1-min creep only

    // --- bias: a 4 %-slow day; correction ramps with cruise fraction ---
    fly(ac, false, 0.96, 0.0, &ferr, &crange, &trange, &tod_late, &eq1, &eq3, &r40);
    printf("  bias:   r(+40min)=%.3f err@25%%=%.0fs err@75%%=%.0fs final=%.0fs max_step=%ld\n",
           r40, eq1, eq3, ferr, g_max_step);
    assert(r40 > 0 && r40 < 0.995);                      // learned we're slow
    assert(eq3 <= eq1 + 30.0);                           // error shrinks with p
    assert(ferr >= 0 && ferr <= 300.0);
    assert(g_max_step <= 1);

    // --- true τ: after the seed, 2700 monotonic seconds must move r_ema by
    // exactly one time constant toward the measured ratio ---
    {
        eta_state_t r2; eta_reset(&r2);
        etap_state_t s2; etap_reset(&s2);
        sched_t s3; t_sched(ac, DLAT, DLON, ALAT, ALON, &s3);
        double cov = s3.climb_d + 300.0, t = 0;          // mid first plateau
        // phase 1: on-profile until the bias seeds AND the made-good window
        // is fully converged at ratio 1.0; phase 2: step to 5 % slow and
        // count 2700 monotonic seconds — one true time constant
        double r0 = -1;
        int n2700 = 0;
        double rT = -1;
        int settle = 0;
        for (int i = 0; i < 9000 && rT < 0; i++) {
            bool slow = s2.r_init && ++settle > 700;     // window (600 s) full
            double v = 460.0 * (slow ? 0.95 : 1.0);
            cov += v * 1.0 / 3600.0;                     // 1 s cadence
            t += 1.0;
            double togo_raw = (s3.ptot - cov) / s3.stretch;
            double la, lo; slerp(1.0 - togo_raw / tot, &la, &lo);
            eta_update(&r2, togo_raw, v / s3.stretch, t0 + t, (uint32_t)(t * 1000));
            etap_update(&s2, ac, la, lo, -1, DLAT, DLON, ALAT, ALON, AELEV,
                        tot, togo_raw, v / s3.stretch, eta_made_good_kt(&r2),
                        t0 + t, (uint32_t)(t * 1000), 1, false);
            // start the τ stopwatch once the measured ratio has fully moved
            // (the 600 s made-good window lags the speed step)
            if (slow && r0 < 0 && settle > 700 + 650) r0 = s2.r_ema;
            else if (r0 > 0 && ++n2700 == 2700) rT = s2.r_ema;
        }
        double expect = 0.95 + (r0 - 0.95) * exp(-2700.0 / 2700.0);
        printf("  tau:    r0=%.4f r(+2700s)=%.4f expected=%.4f\n", r0, rT, expect);
        assert(r0 > 0.96);                               // transient existed
        assert(fabs(rT - expect) < 0.006);
    }

    // --- early-descent latch: proximity-gated, 90 s persistence, one-shot ---
    {
        // NOT latched far out: 2000 NM to go, 3000 ft low, descending
        eta_state_t r2; eta_reset(&r2);
        etap_state_t s2; etap_reset(&s2);
        sched_t s3; t_sched(ac, DLAT, DLON, ALAT, ALON, &s3);
        double cov = s3.climb_d + 200.0, t = 0, alt = s3.init_alt;
        for (int i = 0; i < 60; i++) {                   // 5 min descending
            cov += 440.0 * 5 / 3600; t += 5; alt -= 100;  // -1200 fpm
            double togo_raw = (s3.ptot - cov) / s3.stretch;
            double la, lo; slerp(1.0 - togo_raw / tot, &la, &lo);
            eta_update(&r2, togo_raw, 440, t0 + t, (uint32_t)(t * 1000));
            etap_update(&s2, ac, la, lo, alt, DLAT, DLON, ALAT, ALON, AELEV,
                        tot, togo_raw, 440, eta_made_good_kt(&r2),
                        t0 + t, (uint32_t)(t * 1000), 1, false);
        }
        assert(!s2.latched);                             // proximity gate holds

        // latches near the destination: final plateau, 240 NM to go
        etap_reset(&s2); eta_reset(&r2);
        cov = s3.tod - 110.0; t = 0; alt = s3.final_alt;
        long before = 0;
        bool was_latched = false;
        for (int i = 0; i < 100; i++) {
            cov += 440.0 * 5 / 3600; t += 5;
            if (i > 20) alt -= 125;                      // -1500 fpm from t=100s
            double togo_raw = (s3.ptot - cov) / s3.stretch;
            double la, lo; slerp(1.0 - togo_raw / tot, &la, &lo);
            eta_update(&r2, togo_raw, 440, t0 + t, (uint32_t)(t * 1000));
            etap_out_t oo = etap_update(&s2, ac, la, lo, alt, DLAT, DLON, ALAT, ALON,
                                        AELEV, tot, togo_raw, 440,
                                        eta_made_good_kt(&r2),
                                        t0 + t, (uint32_t)(t * 1000), 1, false);
            if (!was_latched && s2.latched) {
                was_latched = true;
                assert(labs(oo.eta_min - before) <= 10); // no resync lurch
                assert(oo.tod_min == 0);                 // TOD hidden once latched
            }
            if (oo.eta_min) before = oo.eta_min;
        }
        assert(was_latched);
    }

    // --- NCD blip keeps state; teleport resets ---
    {
        eta_state_t ring; eta_reset(&ring);
        etap_reset(&st);
        sched_t s3; t_sched(ac, DLAT, DLON, ALAT, ALON, &s3);
        double cov = s3.ptot / 2, t = 0;
        long before = 0;
        etap_out_t oo = { 0, 0 };
        for (int i = 0; i < 400; i++) {                  // settle mid-cruise
            cov += 460.0 * 5 / 3600;
            t += 5;
            double togo_raw = (s3.ptot - cov) / s3.stretch;
            double la, lo; slerp(1.0 - togo_raw / tot, &la, &lo);
            eta_update(&ring, togo_raw, 460, t0 + t, (uint32_t)(t * 1000));
            oo = etap_update(&st, ac, la, lo, -1, DLAT, DLON, ALAT, ALON, AELEV,
                             tot, togo_raw, 460, eta_made_good_kt(&ring),
                             t0 + t, (uint32_t)(t * 1000), 1, false);
        }
        before = oo.eta_min;
        assert(before > 0);
        for (int i = 0; i < 12; i++) {                   // 60 s NCD blip
            cov += 460.0 * 5 / 3600;
            t += 5;
            oo = etap_update(&st, ac, 0, 0, -1, DLAT, DLON, ALAT, ALON, AELEV,
                             tot, -1, 460, -1, t0 + t, (uint32_t)(t * 1000), 1, false);
            assert(oo.eta_min == 0 && oo.tod_min == 0);
        }
        cov += 460.0 * 5 / 3600; t += 5;                 // recovery: same minute
        double togo_raw = (s3.ptot - cov) / s3.stretch;
        double la, lo; slerp(1.0 - togo_raw / tot, &la, &lo);
        eta_update(&ring, togo_raw, 460, t0 + t, (uint32_t)(t * 1000));
        oo = etap_update(&st, ac, la, lo, -1, DLAT, DLON, ALAT, ALON, AELEV,
                         tot, togo_raw, 460, eta_made_good_kt(&ring),
                         t0 + t, (uint32_t)(t * 1000), 1, false);
        assert(oo.eta_min && labs(oo.eta_min - before) <= 1);
        long pre_jump = oo.eta_min;
        t += 5;                                          // destination change
        oo = etap_update(&st, ac, la, lo, -1, DLAT, DLON, ALAT, ALON, AELEV,
                         tot, togo_raw + 800, 460, -1,
                         t0 + t, (uint32_t)(t * 1000), 1, false);
        assert(oo.eta_min > 0 && oo.eta_min - pre_jump > 30);  // re-snapped
    }

    // --- winds: DJF Pacific jet is a net headwind northbound ---
    {
        etap_state_t sa, sb, sc2; etap_reset(&sa); etap_reset(&sb); etap_reset(&sc2);
        etap_out_t on  = etap_update(&sa, ac, DLAT, DLON, -1, DLAT, DLON, ALAT, ALON,
                                     AELEV, tot, tot, 460, -1, t0, 1000, 1, true);
        etap_out_t off = etap_update(&sb, ac, DLAT, DLON, -1, DLAT, DLON, ALAT, ALON,
                                     AELEV, tot, tot, 460, -1, t0, 1000, 1, false);
        etap_out_t rev = etap_update(&sc2, ac, ALAT, ALON, -1, ALAT, ALON, DLAT, DLON,
                                     52, tot, tot, 460, -1, t0, 1000, 1, true);
        assert(on.eta_min > off.eta_min + 5);            // slower into the jet
        assert(on.eta_min > rev.eta_min + 10);           // asymmetry, same track
        printf("  winds:  NOU->NRT %+ld min vs still air; asymmetry %ld min\n",
               on.eta_min - off.eta_min, on.eta_min - rev.eta_min);
    }

    // --- made-good accessor ---
    {
        eta_state_t ring; eta_reset(&ring);
        assert(eta_made_good_kt(&ring) == -1);
        for (int i = 0; i <= 180; i++)
            eta_update(&ring, 3000 - 480.0 * i * 5 / 3600, 480, t0 + i * 5,
                       (uint32_t)(i * 5000));
        assert(fabs(eta_made_good_kt(&ring) - 480.0) < 5.0);
    }

    printf("test_eta_profile: PASS\n");
    return 0;
}
