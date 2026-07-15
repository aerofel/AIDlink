// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Host replay of the AIDlink ETA pipeline over a real flown track — see
// tools/replay_flight.py for the friendly driver.
//   clang -Imain -O2 -o /tmp/replay host_test/replay_eta.c main/eta.c \
//         main/eta_profile.c main/derive.c main/geo.c main/perfdb.c \
//         main/perfdb_data.c main/airports.c -lm
// Analysis: docs/superpowers/specs/2026-07-15-eta-stability-replay.md.
//
// Mirrors the firmware wiring exactly:
//   poller_task  — 1 Hz poll (CFG default poll_ms=1000), Viasat envelope has
//                  no GS/track -> derive_update() fills them (poller.c)
//   display_task — refresh() every 500 ms: dist to dest, eta_update()
//                  fallback, etap_update() override when a perf profile is
//                  resolved (display.c:584-594); utc from time(NULL) seconds
// Position between track points is linearly interpolated, exactly like
// mock_server.py serves it to the device.
//
// Usage: replay_eta track.csv ORIG DEST ACTYPE [--no-winds] [--no-bias] > out.csv
// track.csv: t_epoch_s,lat,lon,alt_ft (strictly monotonic)
// out.csv:   t_s,dist_nm,gs_kt,mg_kt,fb_min,etap_min,tod_min,disp_min,r_ema,gt_kt
//   --no-winds  climatology off (CFG->winds_enable = false)
//   --no-bias   freeze the cruise bias at 1.0 (pass gs_made_good = -1)
// r_ema = engine's cruise-bias EMA; gt_kt = engine's predicted GS at the
// present position (recomputed here with the same formulas for logging)
#include "eta.h"
#include "eta_profile.h"
#include "derive.h"
#include "geo.h"
#include "perfdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdbool.h>

static int tmu_month(uint32_t utc_s) {
    time_t us = (time_t)utc_s;
    struct tm tmu;
    gmtime_r(&us, &tmu);
    return tmu.tm_mon + 1;
}

bool airports_lookup(const char *code, double *lat, double *lon);
bool airports_lookup_ex(const char *code, double *lat, double *lon, int *elev_ft);

#define MAXPT 8192
static double T[MAXPT], LA[MAXPT], LO[MAXPT], AL[MAXPT], GS[MAXPT], DTG[MAXPT];
static int N;


static void interp(double t, double *lat, double *lon, double *alt) {
    if (t <= T[0]) { *lat = LA[0]; *lon = LO[0]; *alt = AL[0]; return; }
    if (t >= T[N-1]) { *lat = LA[N-1]; *lon = LO[N-1]; *alt = AL[N-1]; return; }
    static int i = 1;
    if (T[i-1] > t) i = 1;
    while (T[i] < t) i++;
    double f = (t - T[i-1]) / (T[i] - T[i-1]);
    *lat = LA[i-1] + f * (LA[i] - LA[i-1]);
    *lon = LO[i-1] + f * (LO[i] - LO[i-1]);
    *alt = AL[i-1] + f * (AL[i] - AL[i-1]);
}

// mirror of eta_profile.c's predicted cruise GS at a position (for logging)
static double predicted_gs(const perf_ac_t *ac, double lat, double lon,
                           double alat, double alon, int month, bool winds) {
    if (!winds) return ac->cruise_kt;
    double u, v;
    perfdb_wind(lat, lon, month, &u, &v);
    double wspd = hypot(u, v) * 1.944;
    double wdir = atan2(-u, -v) * 180.0 / M_PI;
    if (wdir < 0) wdir += 360.0;
    double crs = geo_bearing_deg(lat, lon, alat, alon);
    double d = (wdir - crs) * M_PI / 180.0;
    double swc = (wspd / ac->cruise_kt) * sin(d);
    if (fabs(swc) > 1.0) { double g = ac->cruise_kt * 0.5; return g < 50 ? 50 : g; }
    double gs = ac->cruise_kt * sqrt(1.0 - swc * swc) - wspd * cos(d);
    return gs < 50 ? 50 : gs;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s track.csv ORIG DEST ACTYPE [--no-winds] [--no-bias]\n", argv[0]); return 2; }
    bool winds = true, bias = true;
    double cruise_override = 0;
    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "--no-winds")) winds = false;
        if (!strcmp(argv[i], "--no-bias"))  bias = false;
        if (!strcmp(argv[i], "--cruise") && i + 1 < argc) cruise_override = atof(argv[++i]);
    }
    FILE *fp = fopen(argv[1], "r");
    if (!fp) { perror(argv[1]); return 2; }
    char line[256];
    while (N < MAXPT && fgets(line, sizeof line, fp)) {
        GS[N] = 0;
        int k = sscanf(line, "%lf,%lf,%lf,%lf,%lf", &T[N], &LA[N], &LO[N], &AL[N], &GS[N]);
        if (k >= 4) N++;
    }
    fclose(fp);
    if (N < 2) { fprintf(stderr, "no track\n"); return 2; }

    double olat, olon, alat, alon; int aelev = 0;
    if (!airports_lookup(argv[2], &olat, &olon)) { fprintf(stderr, "orig %s unknown\n", argv[2]); return 2; }
    if (!airports_lookup_ex(argv[3], &alat, &alon, &aelev)) { fprintf(stderr, "dest %s unknown\n", argv[3]); return 2; }
    const perf_ac_t *perf = perfdb_find(argv[4]);
    if (!perf) fprintf(stderr, "note: no perf profile for '%s' — fallback estimator only\n", argv[4]);
    static perf_ac_t custom;
    if (perf && cruise_override > 100) {
        custom = *perf;
        custom.cruise_kt = (uint16_t)cruise_override;
        perf = &custom;
    }
    double tot_nm = geo_dist_nm(olat, olon, alat, alon);

    derive_state_t drv; memset(&drv, 0, sizeof drv); drv.gs_f = -1;
    eta_state_t eta; eta_reset(&eta);
    etap_state_t etap; etap_reset(&etap);

    // pos snapshot as the poller leaves it
    double p_lat = 0, p_lon = 0, p_gs = 0;
    bool have_fix = false;

    long long t0_ms = (long long)(T[0] * 1000.0);
    long long tend_ms = (long long)(T[N-1] * 1000.0);

    printf("t_s,dist_nm,gs_kt,mg_kt,fb_min,etap_min,tod_min,disp_min,r_ema,gt_kt\n");
    for (long long t_ms = t0_ms; t_ms <= tend_ms; t_ms += 500) {
        long long rel_ms = t_ms - t0_ms;

        if (rel_ms % 1000 == 0) {                       // poller: 1 Hz poll
            double lat, lon, alt;
            interp(t_ms / 1000.0, &lat, &lon, &alt);
            double gs, trk; bool have_trk;
            derive_update(&drv, lat, lon, (uint32_t)rel_ms, -1, 0, false,
                          &gs, &trk, &have_trk);
            if (gs > 1500) gs = 1500;                   // poller.c clamp
            p_lat = lat; p_lon = lon; p_gs = gs;
            have_fix = true;
        }

        // display refresh (every 500 ms), utc = whole seconds like time(NULL)
        if (!have_fix) continue;
        uint32_t utc_s = (uint32_t)(t_ms / 1000);
        double dist_nm = geo_dist_nm(p_lat, p_lon, alat, alon);
        long fb = eta_update(&eta, dist_nm, p_gs, (double)utc_s);
        long disp = fb, etap_min = 0, tod_min = 0;
        if (perf && tot_nm > 10) {
            time_t us = (time_t)utc_s;
            struct tm tmu;
            gmtime_r(&us, &tmu);
            etap_out_t po = etap_update(&etap, perf, p_lat, p_lon, alat, alon,
                                        aelev, tot_nm, dist_nm, p_gs,
                                        bias ? eta_made_good_kt(&eta) : -1.0,
                                        (double)utc_s, tmu.tm_mon + 1, winds);
            etap_min = po.eta_min; tod_min = po.tod_min;
            if (po.eta_min > 0) disp = po.eta_min;
        }
        printf("%u,%.2f,%.1f,%.1f,%ld,%ld,%ld,%ld,%.4f,%.1f\n",
               utc_s, dist_nm, p_gs, eta_made_good_kt(&eta),
               fb, etap_min, tod_min, disp, etap.r_ema,
               perf ? predicted_gs(perf, p_lat, p_lon, alat, alon,
                                   tmu_month(utc_s), winds) : 0.0);
    }
    return 0;
}
