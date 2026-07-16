// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Theoretical-profile arrival estimator — see eta_profile.h for the design
// and docs/superpowers/specs/2026-07-16-orthodromic-vertical-eta-design.md.
//
// Experiment flags (replay isolation matrix, 2026-07-16):
//   ETAP_ISO_NO_VERT       legacy vertical shape: cruise at the service
//                          ceiling, descent to the field plus an APPENDED
//                          60 NM approach, no steps, no altitude latch
//   ETAP_STRETCH_APPLY=1   apply the route-stretch factor (see eta_profile.h)
//   ETAP_BIAS_APPLY=1      apply the cruise-bias multiplier
#include "eta_profile.h"
#include "geo.h"
#include <math.h>

#define D2R (M_PI / 180.0)
#define EARTH_NM 3440.065

void etap_reset(etap_state_t *st) {
    st->r_ema = 1.0; st->r_init = false;
    st->have_eta = false; st->have_tod = false;
    st->eta_s = 0; st->tod_s = 0;
    st->shown_eta_min = 0; st->shown_tod_min = 0;
    st->last_now = 0; st->last_dist = 0;
    st->mono_last = 0; st->mono_have = false;
    st->sch_ok = false;
    st->vs_fpm = 0; st->alt_have = false; st->alt_last = 0; st->alt_last_ms = 0;
    st->latch_hold_s = 0; st->latched = false; st->latch_cov = 0; st->latch_alt = 0;
}

// frozen distance-only route stretch (spec 3.2): quartic ramp 1500->5100 NM
double etap_stretch(double raw_total_gc_nm) {
    double x = (raw_total_gc_nm - ETAP_STRETCH_D0_NM) / ETAP_STRETCH_DW_NM;
    if (x < 0) x = 0;
    if (x > 1) x = 1;
    double x2 = x * x;
    return 1.0 + ETAP_STRETCH_MAX_PCT * x2 * x2 / 100.0;
}

// ISA IAS→TAS below the tropopause (Offto parity: TAS = IAS/sqrt(sigma),
// stratosphere temperature clamped)
static double ias_to_tas(double ias_kt, double alt_msl_ft) {
    double T = 288.15 - 0.0019812 * alt_msl_ft;
    if (T < 216.65) T = 216.65;
    return ias_kt * pow(288.15 / T, 2.128);
}

// ISA speed of sound, knots (a0 = 661.47 kt at 288.15 K)
static double isa_sound_kt(double alt_msl_ft) {
    double T = 288.15 - 0.0019812 * alt_msl_ft;
    if (T < 216.65) T = 216.65;
    return 661.47 * sqrt(T / 288.15);
}

// upper-climb TAS at altitude: 300 KIAS capped by the DB climb Mach (spec 4.4)
static double climb_tas(const perf_ac_t *ac, double alt_msl_ft) {
    double v_ias = ias_to_tas(300.0, alt_msl_ft);
    double v_mach = ac->climb_mach * isa_sound_kt(alt_msl_ft);
    return v_ias < v_mach ? v_ias : v_mach;
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

static void push(etap_table_t *tb, double len_nm, double dur_s) {
    if (len_nm <= 0 || tb->n >= ETAP_MAX_BP) return;
    tb->d[tb->n] = tb->d[tb->n - 1] + len_nm;
    tb->t[tb->n] = tb->t[tb->n - 1] + dur_s;
    tb->n++;
}

static double interp(const etap_table_t *tb, double x) {
    if (x <= tb->d[0]) return tb->t[0];
    for (int i = 1; i < tb->n; i++)
        if (x <= tb->d[i]) {
            double span = tb->d[i] - tb->d[i - 1];
            double f = span > 1e-9 ? (x - tb->d[i - 1]) / span : 1.0;
            return tb->t[i - 1] + f * (tb->t[i] - tb->t[i - 1]);
        }
    return tb->t[tb->n - 1];
}

// EMA + minute hysteresis, shared by the arrival and TOD epochs.
// Two conditioning rules from the 2026-07-15 real-flight replay:
//  - hysteresis grows with time-to-go (the estimate hours out is model-
//    limited to tens of minutes; walking every wiggle wastes attention),
//  - the shown minute CREEPS by 1 toward the smoothed epoch instead of
//    re-rounding (ETAP_HYST_S > 60 s made every lround change a 2-min jump).
// Resyncs (first estimate, destination change) still snap directly.
static long condition(double raw, double dt, double now_s,
                      double *sm, bool *have, long *shown) {
    if (!*have || fabs(raw - *sm) > ETAP_RESYNC_S) {
        *have = true;
        *sm = raw;
        *shown = lround(raw / 60.0);
    } else {
        double a = 1.0 - exp(-dt / ETAP_OUT_TAU_S);
        *sm += (raw - *sm) * a;
        double hyst = ETAP_HYST_S;
        double tte_h = (*sm - now_s) / 3600.0;
        if (tte_h > 1.0) hyst += (tte_h - 1.0) * ETAP_HYST_SLOPE;
        if (hyst > ETAP_HYST_CAP_S) hyst = ETAP_HYST_CAP_S;
        if (fabs(*sm - *shown * 60.0) > hyst)
            *shown += (*sm > *shown * 60.0) ? 1 : -1;
    }
    return *shown;
}

// ---- frozen route/vertical schedule (spec 4) -------------------------------
static void build_sched(etap_state_t *st, const perf_ac_t *ac,
                        double olat, double olon, double alat, double alon,
                        double tot_nm) {
    st->stretch = ETAP_STRETCH_APPLY ? etap_stretch(tot_nm) : 1.0;
    st->ptot = tot_nm * st->stretch;
    st->sch_tot = tot_nm;

    double usable = ac->ceiling_ft;
    st->nstep = 0;
#ifndef ETAP_ISO_NO_VERT
    // direction-compatible ceiling: initial GC bearing as semicircular proxy
    double brg = geo_bearing_deg(olat, olon, alat, alon);
    bool east = brg < 180.0;
    int fl = (int)(ac->ceiling_ft / 1000);
    if (((fl & 1) == 1) != east) fl -= 1;
    usable = fl * 1000.0;

    // initial level from the range fraction (calibrated against the DB's
    // OPERATIONAL route-distance semantics — see review note 7)
    double rf = ac->max_range_nm > 100 ? st->ptot / ac->max_range_nm : 0.0;
    if (rf < 0) rf = 0;
    if (rf > 1) rf = 1;
    st->nstep = (int)lround(3.0 * rf * rf);
    if (st->nstep > ETAP_MAX_STEP) st->nstep = ETAP_MAX_STEP;
#endif
    st->init_alt = usable - 2000.0 * st->nstep;
    double floor_alt = usable - 6000.0 > 28000.0 ? usable - 6000.0 : 28000.0;
    if (st->init_alt < floor_alt) st->init_alt = floor_alt;
    if (st->init_alt > usable) st->init_alt = usable;
    st->final_alt = st->init_alt + 2000.0 * st->nstep;
    if (st->final_alt > usable) st->final_alt = usable;

    // climb to the initial level: DB minutes for the two lower segments, the
    // upper segment time scaled by altitude, distance from the ISA/Mach speed
    double d1 = ac->climb1_min / 60.0 * 280.0, t1 = ac->climb1_min * 60.0;
    double d2 = ac->climb2_min / 60.0 * 380.0, t2 = ac->climb2_min * 60.0;
    double span = ac->ceiling_ft - 20000.0;
    if (span < 1000.0) span = 1000.0;
    double t3 = ac->climb3_min * 60.0 * (st->init_alt - 20000.0) / span;
    if (t3 < 0) t3 = 0;
    double v3 = climb_tas(ac, (20000.0 + st->init_alt) / 2.0);
#ifdef ETAP_ISO_NO_VERT
    v3 = ac->climb_mach * 593.7;               // legacy fixed climb-3 TAS
#endif
    st->climb_d = d1 + d2 + t3 / 3600.0 * v3;
    st->climb_t = t1 + t2 + t3;

    // step climbs: same altitude-fraction of the DB upper-climb time each
    st->step_t = ac->climb3_min * 60.0 * 2000.0 / span;
    double steps_d = 0;
    for (int k = 0; k < st->nstep; k++) {
        double mid = st->init_alt + 2000.0 * k + 1000.0;
        st->step_d[k] = st->step_t / 3600.0 * climb_tas(ac, mid);
        steps_d += st->step_d[k];
    }

    st->desc_d = st->final_alt / 300.0;
    double level = st->ptot - st->climb_d - steps_d - st->desc_d;
#ifdef ETAP_ISO_NO_VERT
    level -= ETAP_APPROACH_NM;                 // legacy: approach appended
#endif
    if (level < 0 && st->nstep > 0) {          // short leg: no steps, retry flat
        st->nstep = 0;
        st->init_alt = st->final_alt = usable;
        double t3f = ac->climb3_min * 60.0 * (usable - 20000.0) / span;
        st->climb_d = d1 + d2 + t3f / 3600.0 * climb_tas(ac, (20000.0 + usable) / 2.0);
        st->climb_t = t1 + t2 + t3f;
        st->desc_d = usable / 300.0;
        level = st->ptot - st->climb_d - st->desc_d;
#ifdef ETAP_ISO_NO_VERT
        level -= ETAP_APPROACH_NM;
#endif
    }
    if (level < 0) level = 0;

    // equal-distance plateaus with one step between adjacent ones
    double plat = level / (st->nstep + 1);
    double at = st->climb_d;
    for (int k = 0; k < st->nstep; k++) {
        at += plat;
        st->step_at[k] = at;
        at += st->step_d[k];
    }
    st->tod_nm = st->climb_d + level + steps_d;   // == ptot - desc_d (+60 legacy)
    st->sch_ok = true;
}

etap_out_t etap_update(etap_state_t *st, const perf_ac_t *ac,
                       double lat, double lon, double alt_ft,
                       double olat, double olon,
                       double alat, double alon,
                       int dest_elev_ft, double tot_nm, double dist_to_go_nm,
                       double gs_inst_kt, double gs_made_good_kt,
                       double now_s, uint32_t mono_ms, int month, bool winds) {
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
    // dt from the monotonic clock (the 1 s epoch grain at 2 Hz used to
    // over-integrate the filters by ~1.5x — 2026-07-16 audit)
    double dt = st->mono_have ? (mono_ms - st->mono_last) / 1000.0 : 0.5;
    if (dt <= 0) dt = 0.001;
    st->mono_last = mono_ms; st->mono_have = true;
    st->last_now = now_s;
    st->last_dist = dist_to_go_nm;

    double gs_ref = gs_made_good_kt > gs_inst_kt ? gs_made_good_kt : gs_inst_kt;
    if (gs_ref < ETAP_MIN_GS_KT) return out;           // taxi/hold: keep state

    // ---- frozen schedule ---------------------------------------------------
    if (!st->sch_ok || fabs(tot_nm - st->sch_tot) > 1.0)
        build_sched(st, ac, olat, olon, alat, alon, tot_nm);

    double tas = ac->cruise_kt;
    double dist_eff = dist_to_go_nm * st->stretch;
    double covered = st->ptot - dist_eff;
    if (covered < 0) covered = 0;                      // early off-track legs
    if (covered > st->ptot) covered = st->ptot;

    // scheduled plateau altitude + phase at the present covered distance
    int plateau = 0;
    bool in_step = false;
    for (int k = 0; k < st->nstep; k++) {
        if (covered >= st->step_at[k] + st->step_d[k]) plateau = k + 1;
        else if (covered >= st->step_at[k]) { in_step = true; break; }
    }
    double sched_alt = st->init_alt + 2000.0 * plateau;
    bool level_cruise = !in_step && covered >= st->climb_d && covered < st->tod_nm;

    // ---- live-altitude descent latch (spec 5.2 + proximity gate) -----------
#ifndef ETAP_ISO_NO_VERT
    if (alt_ft >= 0) {
        if (st->alt_have) {
            double adt = (mono_ms - st->alt_last_ms) / 1000.0;
            if (adt >= 1.0) {
                double vs = (alt_ft - st->alt_last) / adt * 60.0;
                st->vs_fpm += 0.3 * (vs - st->vs_fpm);
                st->alt_last = alt_ft; st->alt_last_ms = mono_ms;
            }
        } else {
            st->alt_have = true; st->alt_last = alt_ft; st->alt_last_ms = mono_ms;
        }
        if (!st->latched && level_cruise) {
            bool cond = alt_ft < sched_alt - ETAP_LATCH_BELOW_FT &&
                        st->vs_fpm < ETAP_LATCH_VS_FPM &&
                        dist_eff < ETAP_LATCH_PROX * (alt_ft / 300.0) + ETAP_APPROACH_NM;
            st->latch_hold_s = cond ? st->latch_hold_s + dt : 0;
            if (st->latch_hold_s >= ETAP_LATCH_HOLD_S) {
                st->latched = true;
                st->latch_cov = covered;
                st->latch_alt = alt_ft;
            }
        }
    }
#else
    (void)alt_ft; (void)sched_alt;
#endif

    // ---- cruise bias: measured CLOSURE vs predicted closure ----------------
    // eta_made_good_kt() is a closure rate (dist-to-go deltas); the predicted
    // path GS divided by the stretch is its model equivalent (spec 6.2).
    // Eligibility: level plateau only — never climb, steps, descent, approach.
    double p = 0.0;                                    // fraction of cruise flown
    double cruise_span = st->tod_nm - st->climb_d;
    if (cruise_span > 20.0) {
        p = (covered - st->climb_d) / cruise_span;
        if (p < 0) p = 0;
        if (p > 1) p = 1;
    }
    if (level_cruise && !st->latched && cruise_span > 20.0 &&
        gs_made_good_kt >= ETAP_MIN_GS_KT) {
        double gt = tas;                               // predicted GS right here
        if (winds) {
            double wspd, wdir;
            met_wind(lat, lon, month, &wspd, &wdir);
            gt = wind_gs(tas, geo_bearing_deg(lat, lon, alat, alon), wdir, wspd);
        }
        double r_raw = gs_made_good_kt * st->stretch / gt;
        if (!st->r_init) { st->r_ema = r_raw; st->r_init = true; }
        else {
            double a = 1.0 - exp(-dt / ETAP_BIAS_TAU_S);
            st->r_ema += (r_raw - st->r_ema) * a;
        }
        if (st->r_ema < ETAP_BIAS_MIN) st->r_ema = ETAP_BIAS_MIN;
        if (st->r_ema > ETAP_BIAS_MAX) st->r_ema = ETAP_BIAS_MAX;
    }
    // r_ema keeps learning for diagnostics either way; the multiplier only
    // engages when ETAP_BIAS_APPLY is set (leverage-scaled by p)
    double bias = ETAP_BIAS_APPLY ? 1.0 + (st->r_ema - 1.0) * p : 1.0;

    // ---- breakpoint table (effective dist-from-origin -> cumulative time) --
    etap_table_t *tb = &st->tb;
    tb->d[0] = 0; tb->t[0] = 0; tb->n = 1;
    double d1 = ac->climb1_min / 60.0 * 280.0;
    double d2 = ac->climb2_min / 60.0 * 380.0;
    push(tb, d1, ac->climb1_min * 60.0);
    push(tb, d2, ac->climb2_min * 60.0);
    push(tb, st->climb_d - d1 - d2, st->climb_t - (ac->climb1_min + ac->climb2_min) * 60.0);

    double tod = st->latched ? st->latch_cov : st->tod_nm;
    double rem_from = covered > st->climb_d ? covered : st->climb_d;
    if (rem_from > tod) rem_from = tod;
    // flown level flight at plain TAS (cancels out of every remaining-time
    // difference — only segments ahead of `covered` shape the output)
    push(tb, rem_from - st->climb_d, (rem_from - st->climb_d) / tas * 3600.0);

    // remaining level flight: wind-segmented plateaus + frozen step climbs
    int wind_budget = 24;
    double pos = rem_from;
    for (int k = 0; k <= st->nstep && pos < tod - 0.5; k++) {
        double seg_end = tod;                          // last plateau
        if (!st->latched && k < st->nstep &&
            st->step_at[k] + st->step_d[k] > pos) {
            // plateau piece up to step k, then the (partial) step
            if (st->step_at[k] < seg_end) seg_end = st->step_at[k];
        } else if (k < st->nstep) {
            continue;                                  // step k fully behind
        }
        double len = seg_end - pos;
        if (len > 0.5) {
            int nseg = (int)ceil(len / 200.0);
            if (nseg < 1) nseg = 1;
            if (nseg > wind_budget) nseg = wind_budget > 0 ? wind_budget : 1;
            wind_budget -= nseg;
            double seglen = len / nseg;
            for (int i = 0; i < nseg; i++) {
                double gs = tas;
                if (winds && dist_eff > 1.0) {
                    double f = (pos + (i + 0.5) * seglen - covered) / dist_eff;
                    if (f < 0) f = 0;
                    if (f > 1) f = 1;
                    double mlat, mlon, wspd, wdir;
                    gc_slerp(lat, lon, alat, alon, f, &mlat, &mlon);
                    met_wind(mlat, mlon, month, &wspd, &wdir);
                    gs = wind_gs(tas, geo_bearing_deg(mlat, mlon, alat, alon), wdir, wspd);
                }
                gs *= bias;
                push(tb, seglen, seglen / gs * 3600.0);
            }
            pos = seg_end;
        }
        if (!st->latched && k < st->nstep && pos < tod - 0.1) {
            double s1 = st->step_at[k] + st->step_d[k];
            double part = s1 - pos;                    // partial if mid-step
            if (part > 0.05) {
                push(tb, part, st->step_t * (part / st->step_d[k]));
                pos = s1;
            }
        }
    }

    // descent: IAS schedule integrated to 60 NM out; staged approach OVERLAYS
    // the last 60 NM (legacy shape appends it instead — ETAP_ISO_NO_VERT)
    double top_alt = st->latched ? st->latch_alt : st->final_alt;
    double desc_total = st->latched ? (st->ptot - st->latch_cov) : (st->ptot - st->tod_nm);
#ifdef ETAP_ISO_NO_VERT
    double low_alt = dest_elev_ft;
    double dud = desc_total - ETAP_APPROACH_NM;
#else
    double low_alt = dest_elev_ft + ETAP_APPROACH_NM * 300.0;   // 3°: 18000 AGL
    double dud = desc_total - ETAP_APPROACH_NM;
#endif
    if (dud > 0.5) {
        for (int i = 0; i < 20; i++) {
            double q = (i + 0.5) / 20.0;
            double alt = top_alt - q * (top_alt - low_alt);
            double agl = alt - dest_elev_ft;
            double ias = agl > 10000.0 ? 290.0
                       : agl > 4000.0  ? 180.0 + (agl - 4000.0) / 6000.0 * 70.0
                                       : 140.0 + agl / 4000.0 * 40.0;
            push(tb, dud / 20.0, dud / 20.0 / ias_to_tas(ias, alt) * 3600.0);
        }
    }

    // approach: staged speeds over the last 60 NM, single arrival wind
    double awspd = 0, awdir = 0, acrs = 0;
    if (winds) {
        double f60 = dist_eff > ETAP_APPROACH_NM
                   ? (dist_eff - ETAP_APPROACH_NM) / dist_eff : 0.0;
        double plat_, plon_;
        gc_slerp(lat, lon, alat, alon, f60, &plat_, &plon_);
        acrs = geo_bearing_deg(plat_, plon_, alat, alon);
        met_wind(alat, alon, month, &awspd, &awdir);
    }
    static const double ab[6] = { 60, 40, 25, 15, 8, 0 };
    static const double as[5] = { 390, 280, 220, 180, 140 };
    for (int i = 0; i < 5; i++) {
        double sp = winds ? wind_gs(as[i], acrs, awdir, awspd) : as[i];
        push(tb, ab[i] - ab[i + 1], (ab[i] - ab[i + 1]) / sp * 3600.0);
    }

    // whole-profile floor (Offto parity: never faster than 0.8 x direct)
    double t_end = tb->t[tb->n - 1];
    double t_floor = 0.8 * st->ptot / tas * 3600.0;
    double scale = (t_end > 0 && t_end < t_floor) ? t_floor / t_end : 1.0;

    // ---- outputs ------------------------------------------------------------
    double at_cov = interp(tb, covered);
    out.eta_min = condition(now_s + (t_end - at_cov) * scale, dt, now_s,
                            &st->eta_s, &st->have_eta, &st->shown_eta_min);
    if (covered < tod - 1.0 && cruise_span > 0.5 && !st->latched) {
        out.tod_min = condition(now_s + (interp(tb, tod) - at_cov) * scale, dt, now_s,
                                &st->tod_s, &st->have_tod, &st->shown_tod_min);
    } else {
        st->have_tod = false;                          // TOD passed: hide, stay 0
        st->shown_tod_min = 0;
    }
    return out;
}
