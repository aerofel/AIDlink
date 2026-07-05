// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Shared ownship position state. Written by the poller / simulator (M4),
// read by the ADBP server (M3) and the web status page. All access goes through
// a mutex-guarded snapshot so producers and consumers never tear a read.
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool     valid;         // have a usable fix
    bool     simulated;     // fix came from the emulator
    bool     have_track;    // track/heading is meaningful
    bool     fixed;         // emulator override: do not dead-reckon
    bool     service_avail; // upstream position service reachable
    double   lat, lon;      // degrees
    double   alt_ft;        // feet
    double   gs_kt;         // knots
    double   track_deg;     // degrees true
    uint64_t utc_ms;        // fix time, ms since epoch (0 = unknown)
    uint32_t last_fix_ms;   // monotonic ms (esp_timer) of last fix — freshness gate
    char     flight[16];
    char     tail[12];
    char     orig[8];
    char     dest[8];
} pos_state_t;

// Initialize the mutex (call once at boot before any get/set).
void pos_init(void);

// Thread-safe snapshot into *out.
void pos_get(pos_state_t *out);

// Thread-safe replace of the whole state.
void pos_set(const pos_state_t *in);

// Mark the fix invalid / service unavailable (e.g. on stale timeout) without
// disturbing the last known lat/lon.
void pos_mark_stale(void);
