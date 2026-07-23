// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// See flightview.h. This is the former body of display.c's refresh(), lifted
// out verbatim in behaviour and stripped of every LVGL call, so that both the
// colour LCD and the mono OLED render the same numbers.
#include "flightview.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include "esp_timer.h"
#include "pos.h"
#include "geo.h"
#include "airports.h"
#include "tzdb.h"
#include "netcore.h"
#include "eta.h"
#include "eta_profile.h"
#include "perfdb.h"
#include "buildnum.h"

static const aidlink_cfg_t *CFG;
static eta_state_t  s_eta;    // made-good estimator (fallback + sample ring)
static etap_state_t s_etap;   // theoretical-profile estimator

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void flightview_init(const aidlink_cfg_t *cfg)
{
    CFG = cfg;
    memset(&s_eta, 0, sizeof s_eta);
    memset(&s_etap, 0, sizeof s_etap);
}

void flightview_build(flightview_t *v)
{
    memset(v, 0, sizeof *v);
    v->dist_nm = -1;
    v->ip = CFG->ap_ip;
    v->buildnum = FW_BUILDNUM;

    pos_state_t p;
    pos_get(&p);
    v->valid = p.valid;
    v->simulated = p.simulated;

    // --- identity ---------------------------------------------------------
    v->have_id = p.tail[0] || CFG->ac_tail[0];
    snprintf(v->tail, sizeof v->tail, "%s", p.tail[0] ? p.tail : CFG->ac_tail);

    // resolved performance-DB type code (portal choice or feed pre-select) —
    // never the raw feed string; blank when no profile is selected
    const perf_ac_t *perf = perfdb_find(CFG->perf_type);
    snprintf(v->actype, sizeof v->actype, "%s", perf ? perf->type : "");

    // flight number split: leading letters are the airline prefix
    const char *fl = p.flight;
    int pre = 0;
    while (fl[pre] && !isdigit((unsigned char)fl[pre]) && pre < 7) pre++;
    memcpy(v->fl_prefix, fl, pre);
    v->fl_prefix[pre] = 0;
    snprintf(v->fl_number, sizeof v->fl_number, "%s", fl + pre);

    // --- route ------------------------------------------------------------
    v->have_route = p.orig[0] || p.dest[0];
    if (v->have_route) {
        // prefer IATA for display (NOU>NRT reads better than NWWW>RJAA on a
        // narrow panel); ICAO when the gazetteer has no IATA, raw code last
        const char *o = airports_iata(p.orig); if (!o) o = airports_icao(p.orig);
        if (!o) o = p.orig[0] ? p.orig : "----";
        const char *d = airports_iata(p.dest); if (!d) d = airports_icao(p.dest);
        if (!d) d = p.dest[0] ? p.dest : "----";
        snprintf(v->orig, sizeof v->orig, "%s", o);
        snprintf(v->dest, sizeof v->dest, "%s", d);
    } else {
        v->route_placeholder = p.valid ? "- - -" : "NO POSITION";
    }

    // --- position ---------------------------------------------------------
    if (p.valid) {
        v->have_pos = true;
        double av = fabs(p.lat);
        snprintf(v->lat_h, sizeof v->lat_h, "%s", p.lat >= 0 ? "N" : "S");
        snprintf(v->lat_v, sizeof v->lat_v, "%02d\xC2\xB0%05.2f",
                 (int)av, (av - (int)av) * 60.0);
        av = fabs(p.lon);
        snprintf(v->lon_h, sizeof v->lon_h, "%s", p.lon >= 0 ? " E" : " W");
        snprintf(v->lon_v, sizeof v->lon_v, "%03d\xC2\xB0%05.2f",
                 (int)av, (av - (int)av) * 60.0);

        // altitude rounded to 10 ft — raw feet jitter isn't information
        char buf[16];
        snprintf(buf, sizeof buf, "%d", (int)(lround(p.alt_ft / 10.0) * 10));
        size_t bl = strlen(buf);
        if (bl > 2) {
            memcpy(v->alt_lead, buf, bl - 2);
            v->alt_lead[bl - 2] = 0;
            // exactly two trailing digits — snprintf here can't be proven
            // bounded by the compiler and trips -Werror=format-truncation
            v->alt_last2[0] = buf[bl - 2];
            v->alt_last2[1] = buf[bl - 1];
            v->alt_last2[2] = 0;
        } else {
            snprintf(v->alt_lead, sizeof v->alt_lead, "%s", buf);
        }
    }

    // --- distance and trip completion -------------------------------------
    double alat = 0, alon = 0, olat = 0, olon = 0, tot_nm = -1;
    int dest_elev = 0;
    if (p.valid && p.dest[0] && airports_lookup_ex(p.dest, &alat, &alon, &dest_elev))
        v->dist_nm = geo_dist_nm(p.lat, p.lon, alat, alon);

    if (v->dist_nm >= 0 && p.orig[0] && airports_lookup(p.orig, &olat, &olon))
        tot_nm = geo_dist_nm(olat, olon, alat, alon);

    v->have_trip = tot_nm > 10;
    if (v->have_trip) {
        double pct = (1.0 - v->dist_nm / tot_nm) * 100.0;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        v->trip_pct = pct;
    }

    if (v->dist_nm >= 0) {
        snprintf(v->nm_val, sizeof v->nm_val, "%d", (int)lround(v->dist_nm));
        v->nm_has_unit = true;
    } else if (p.valid && p.simulated) {
        snprintf(v->nm_val, sizeof v->nm_val, "SIM");
    }

    // --- UTC --------------------------------------------------------------
    // Prefer the system clock (SNTP / HTTP-Date disciplined, see poller.c),
    // else tick the last fix's UTC timestamp forward.
    uint64_t utc = 0;
    time_t st = time(NULL);
    if (st > 1750000000) utc = (uint64_t)st * 1000ULL;
    else if (p.utc_ms)   utc = p.utc_ms + (now_ms() - p.last_fix_ms);
    uint32_t utc_s = (uint32_t)(utc / 1000);

    struct tm tm;
    if (utc) {
        v->have_utc = true;
        time_t u = (time_t)(utc / 1000);
        gmtime_r(&u, &tm);
        snprintf(v->utc_hms, sizeof v->utc_hms, "%02d:%02d:%02d",
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    // --- local time at the current position -------------------------------
    // Embedded IANA-derived grid (handles DST and :30/:45 offsets); tzdb falls
    // back to a solar estimate internally.
    int off_min = 0;
    if (p.valid || p.lat != 0 || p.lon != 0) {
        off_min = tzdb_offset_min(p.lat, p.lon, utc_s ? utc_s : 1767225600u);
        v->have_tz = true;
    }
    if (v->have_tz) {
        if (off_min % 60)
            snprintf(v->tz_label, sizeof v->tz_label, "UTC%+d:%02d",
                     off_min / 60, abs(off_min % 60));
        else if (off_min == 0)
            snprintf(v->tz_label, sizeof v->tz_label, "UTC");
        else
            snprintf(v->tz_label, sizeof v->tz_label, "UTC%+d", off_min / 60);
    }
    if (utc && v->have_tz) {
        v->have_local = true;
        time_t local = (time_t)(utc / 1000) + (time_t)off_min * 60;
        gmtime_r(&local, &tm);
        snprintf(v->local_hms, sizeof v->local_hms, "%02d:%02d:%02d",
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    // --- arrival estimate + top of descent --------------------------------
    // The made-good estimator always runs (it owns the sample ring and is the
    // fallback); when an aircraft profile is resolved and the route is known,
    // the theoretical-profile estimator overrides it and adds TOD.
    // Epochs anchor the displayed times; the monotonic clock drives the filter
    // dt (whole-second epochs at 2 Hz over-integrated the EMAs).
    uint32_t mono = now_ms();
    long eta_min = eta_update(&s_eta, v->dist_nm, p.gs_kt, utc ? utc / 1000.0 : 0, mono);
    long tod_min = 0;
    if (perf && utc && v->dist_nm >= 0 && tot_nm > 10) {
        time_t us = (time_t)(utc / 1000);
        struct tm tmu;
        gmtime_r(&us, &tmu);
        etap_out_t po = etap_update(&s_etap, perf, p.lat, p.lon,
                                    p.valid ? p.alt_ft : -1,
                                    olat, olon, alat, alon,
                                    dest_elev, tot_nm, v->dist_nm, p.gs_kt,
                                    eta_made_good_kt(&s_eta), utc / 1000.0,
                                    mono, tmu.tm_mon + 1, CFG->winds_enable);
        if (po.eta_min > 0) { eta_min = po.eta_min; tod_min = po.tod_min; }
    }

    if (eta_min > 0) {
        v->have_eta = true;
        time_t et = (time_t)eta_min * 60;
        gmtime_r(&et, &tm);
        snprintf(v->eta_hhmm, sizeof v->eta_hhmm, "%02d:%02d", tm.tm_hour, tm.tm_min);
    }
    // destination-local ETA: timezone AT the arrival airport, evaluated at the
    // arrival epoch so DST lands on the right side of a transition
    if (eta_min > 0 && v->dist_nm >= 0) {
        v->have_leta = true;
        time_t el = (time_t)eta_min * 60
                  + (time_t)tzdb_offset_min(alat, alon, (uint32_t)(eta_min * 60)) * 60;
        gmtime_r(&el, &tm);
        snprintf(v->leta_hhmm, sizeof v->leta_hhmm, "%02d:%02d", tm.tm_hour, tm.tm_min);
    }
    if (tod_min > 0) {
        v->have_tod = true;
        time_t tt = (time_t)tod_min * 60;
        gmtime_r(&tt, &tm);
        snprintf(v->tod_hhmm, sizeof v->tod_hhmm, "%02d:%02d", tm.tm_hour, tm.tm_min);
    }
}

void flightview_status(fv_status_t *s)
{
    memset(s, 0, sizeof *s);
    uint32_t now = now_ms();

    if (netcore_scanning()) {
        s->link = FV_LINK_SCANNING;
        s->blink_on = (now / 150) % 2;             // fast, ~3 Hz
    } else if (netcore_sta_up(NULL)) {
        // banded level with 3 dB hysteresis per boundary so a hovering RSSI
        // doesn't flicker between levels
        static bool ge_med, ge_str;
        int rssi = netcore_sta_rssi();
        if (rssi >= -67) ge_med = true; else if (rssi <= -73) ge_med = false;
        if (rssi >= -57) ge_str = true; else if (rssi <= -63) ge_str = false;
        s->link = FV_LINK_UP;
        s->bars = ge_str ? 3 : (ge_med ? 2 : 1);
        s->blink_on = true;
    } else {
        s->link = FV_LINK_DOWN;
        s->blink_on = (now / 600) % 2;             // slow, ~0.8 Hz
    }

    s->internet = netcore_inet_up();

    static uint32_t feed_seq, feed_until;
    uint32_t seq = pos_fix_seq();
    if (seq != feed_seq) { feed_seq = seq; feed_until = now + 180; }
    s->feed_active = now < feed_until;
}
