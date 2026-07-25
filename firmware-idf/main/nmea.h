// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Pure NMEA 0183 parser. Deliberately free of ESP-IDF and FreeRTOS types so it
// builds and unit-tests on the host with plain clang, exactly like geo.c,
// derive.c and config_util.c. All GNSS knowledge lives here; gps.c only moves
// bytes and owns the hardware.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// GSA field 2. Values match the NMEA encoding so they can be reported verbatim.
typedef enum { NMEA_FIX_NONE = 1, NMEA_FIX_2D = 2, NMEA_FIX_3D = 3 } nmea_fix_t;

typedef struct {
    nmea_fix_t fix;
    bool     have_pos;        // the most recent GGA carried a usable lat/lon
    bool     rmc_valid;       // most recent RMC status was 'A'
    double   lat, lon;        // degrees, north and east positive
    double   alt_m;           // GGA altitude above MSL, metres
    double   hdop;
    double   gs_kt, track_deg;
    uint64_t utc_ms;          // epoch ms from RMC date+time; 0 when unknown
    int      sats_used;       // GGA field 7
    int      sats_view;       // sum of the per-talker GSV counts
    int      sats_gps, sats_glo, sats_gal, sats_bds, sats_qzss;
} nmea_state_t;

// Zero the state. Call once at start, and whenever counts should be dropped.
void nmea_reset(nmea_state_t *s);

// True when the "$....*HH" checksum matches. This is the ONLY validity gate —
// nmea_line() assumes its input already passed here.
bool nmea_checksum_ok(const char *line);

// Parse one complete sentence (no CR/LF, no surrounding whitespace).
// Returns true when the sentence was understood and something was stored.
bool nmea_line(nmea_state_t *s, const char *line);
