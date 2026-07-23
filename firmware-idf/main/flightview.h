// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Board-independent flight view model.
//
// Everything the onboard displays show is derived here ONCE, from pos + the
// ETA estimators + the airport gazetteer + the timezone grid, and handed to a
// board's layout as plain preformatted data. Layouts place glyphs; they do no
// arithmetic. That is what keeps the 320x170 colour LCD (Board 3) and the
// 128x64 mono OLED (Board 4) from drifting apart as either one is edited.
//
// No LVGL, no esp_lcd: this file compiles on every target, including the
// classic ESP32 that has no display at all.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "config.h"

// Slow-changing content, rebuilt at ~2 Hz.
typedef struct {
    // --- identity row -----------------------------------------------------
    bool have_id;              // false -> layouts show the splash row instead
    char tail[12];             // "F-ONEO" (feed, else configured)
    char actype[8];            // resolved perf-DB code, "" when none
    char fl_prefix[8];         // "ACI"  (airline letters, dimmed)
    char fl_number[12];        // "740"  (number + suffix, bright)

    // --- route ------------------------------------------------------------
    bool valid;                // fix is valid (layouts grey the route if not)
    bool simulated;
    bool have_route;
    char orig[8], dest[8];     // display codes, IATA preferred over ICAO
    const char *route_placeholder;   // used when have_route is false

    // --- position ---------------------------------------------------------
    bool have_pos;
    char lat_h[3], lat_v[16];  // "N" | "22°34.12"
    char lon_h[4], lon_v[16];  // " E" | "110°10.23"
    char alt_lead[16];         // "310"  (bright: reads as the flight level)
    char alt_last2[4];         // "00"   (dimmed)

    // --- distance / trip completion ---------------------------------------
    double dist_nm;            // < 0 when unknown
    bool   have_trip;
    double trip_pct;           // 0..100
    char   nm_val[12];         // "4300", or "SIM", or ""
    bool   nm_has_unit;        // false for the SIM placeholder

    // --- times (all preformatted; layouts never call gmtime) --------------
    bool have_utc;   char utc_hms[12];    // "13:02:45"
    bool have_tz;    char tz_label[20];   // "UTC+11" (sized for %+d worst case)
    bool have_local; char local_hms[12];  // "23:50:12" at the current position
    bool have_tod;   char tod_hhmm[8];    // top of descent, UTC
    bool have_eta;   char eta_hhmm[8];    // arrival, UTC
    bool have_leta;  char leta_hhmm[8];   // arrival, destination-local

    // --- splash row (shown until the feed supplies an identity) -----------
    const char *ip;
    int         buildnum;
} flightview_t;

// Fast-changing status, sampled every display tick so indicators can blink.
typedef enum { FV_LINK_DOWN, FV_LINK_SCANNING, FV_LINK_UP } fv_link_t;

typedef struct {
    fv_link_t link;
    int  bars;        // 1..3, hysteresis-banded RSSI (only when link == UP)
    bool blink_on;    // precomputed blink phase for the DOWN/SCANNING states
    bool internet;    // netcore's frugal reachability probe
    bool feed_active; // a fix arrived within the last ~180 ms
} fv_status_t;

// Call once before the first build (stores cfg, zeroes the estimators).
void flightview_init(const aidlink_cfg_t *cfg);

// Rebuild the slow model. Safe to call from the display task only.
void flightview_build(flightview_t *v);

// Sample the fast status. Cheap; call every tick.
void flightview_status(fv_status_t *s);
