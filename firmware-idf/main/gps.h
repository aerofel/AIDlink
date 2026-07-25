// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Wired GNSS receiver. Owns UART1 and the timepulse input, feeds bytes to the
// pure parser in nmea.c, and publishes a mutex-guarded snapshot.
//
// This module has NO source policy: it never writes pos.h. poller.c arbitrates
// between this and the Wi-Fi feed and stays the single writer of pos_state_t.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "nmea.h"

typedef struct {
    bool       present;       // latched on the first checksum-valid sentence
    nmea_fix_t fix;
    int        sats_used, sats_view;
    int        sats_gps, sats_glo, sats_gal, sats_bds, sats_qzss;   // in view
    int        used_gps, used_glo, used_gal, used_bds, used_qzss;   // in solution
    double     hdop;
    double     lat, lon, alt_ft, gs_kt, track_deg;
    uint64_t   utc_ms;
    uint32_t   last_fix_ms;   // monotonic ms of the last usable positional fix
    uint32_t   last_rx_ms;    // monotonic ms of any checksum-valid sentence
    uint32_t   pps_edges;     // rising edges since boot
    uint32_t   pps_interval_us;
    uint32_t   csum_errors;   // dropped sentences — a wiring/EMI health gauge
} gps_state_t;

// True when this board declares GNSS pins. Everything below is inert if false.
bool gps_has_hw(void);

// Start the receiver task. Safe (and free) to call on boards without GNSS.
void gps_start(void);

// Thread-safe snapshot.
void gps_get(gps_state_t *out);
