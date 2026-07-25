// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Host unit test for the pure NMEA parser. Every fixture below is a REAL
// sentence captured from the receiver on Board 3 on 2026-07-25 (see LEARNING.md)
// — no invented data, so a regression here means the parser stopped handling
// what the actual hardware emits.
//
//   clang -Imain -o /tmp/t host_test/test_nmea.c main/nmea.c -lm && /tmp/t
#include "nmea.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define NEAR(a, b) (fabs((a) - (b)) < 1e-6)

static void test_checksum(void) {
    assert(nmea_checksum_ok("$GNGGA,,,,,,0,00,99.99,,,,,,*56"));
    assert(nmea_checksum_ok("$GNRMC,,V,,,,,,,,,,N,V*37"));
    assert(nmea_checksum_ok("$GBGSV,1,1,01,33,,,28,1*7D"));
    // lower-case hex is legal
    assert(nmea_checksum_ok("$GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,4*36"));

    assert(!nmea_checksum_ok("$GNGGA,,,,,,0,00,99.99,,,,,,*00"));  // wrong sum
    assert(!nmea_checksum_ok("$GNGGA,,,,,,0,00,99.99,,,,,,"));     // no '*'
    assert(!nmea_checksum_ok("GNGGA,,,,,,0,00,99.99*56"));         // no '$'
    assert(!nmea_checksum_ok("$GNGGA*5"));                         // half a byte
    assert(!nmea_checksum_ok("$GNGGA*ZZ"));                        // not hex
    assert(!nmea_checksum_ok(""));
    assert(!nmea_checksum_ok("$"));
}

static void test_gga_no_fix(void) {
    nmea_state_t s;
    nmea_reset(&s);
    // The state the receiver sat in all afternoon before the antenna went outside.
    assert(nmea_line(&s, "$GNGGA,,,,,,0,00,99.99,,,,,,*56"));
    assert(!s.have_pos);
    assert(s.sats_used == 0);
}

static void test_gga_3d_fix(void) {
    nmea_state_t s;
    nmea_reset(&s);
    // Nouméa, the fix that finally came in at 01:29:52Z.
    assert(nmea_line(
        &s, "$GNGGA,012952.00,2217.59913,S,16626.35477,E,1,06,1.30,22.3,M,56.6,M,,*66"));
    assert(s.have_pos);
    assert(s.sats_used == 6);
    assert(NEAR(s.hdop, 1.30));
    assert(NEAR(s.alt_m, 22.3));
    // 22°17.59913' S -> -22.293318...; sign must follow the hemisphere field
    assert(s.lat < -22.29331 && s.lat > -22.29333);
    assert(s.lon > 166.43924 && s.lon < 166.43926);
}

static void test_gsa_fix_dimension(void) {
    nmea_state_t s;
    nmea_reset(&s);
    // GGA establishes THAT there is a fix; GSA only refines it to 2D or 3D.
    assert(nmea_line(&s, "$GNGGA,012952.00,2217.59913,S,16626.35477,E,1,06,1.30,22.3,M,56.6,M,,*66"));
    assert(nmea_line(&s, "$GNGSA,A,3,13,41,08,23,33,,,,,,,,3.36,1.30,3.10,4*0C"));
    assert(s.fix == NMEA_FIX_3D);
    assert(NEAR(s.hdop, 1.30));

    nmea_reset(&s);
    assert(nmea_line(&s, "$GNGGA,012952.00,2217.59913,S,16626.35477,E,1,06,1.30,22.3,M,56.6,M,,*66"));
    assert(nmea_line(&s, "$GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,4*36"));
    assert(s.fix == NMEA_FIX_2D);   // GGA still claims a fix, GSA gives no dimension

    nmea_reset(&s);
    assert(nmea_line(&s, "$GNGGA,012952.00,2217.59913,S,16626.35477,E,1,06,1.30,22.3,M,56.6,M,,*66"));
    assert(nmea_line(&s, "$GNGSA,A,2,13,,,,,,,,,,,,5.00,2.00,3.00,1*06"));
    assert(s.fix == NMEA_FIX_2D);

    // GSA alone, with no GGA ever seen, must NOT claim a fix.
    nmea_reset(&s);
    assert(nmea_line(&s, "$GNGSA,A,3,13,41,08,23,33,,,,,,,,3.36,1.30,3.10,4*0C"));
    assert(s.fix == NMEA_FIX_NONE);
}

// Regression for a bug seen live on Board 3: the receiver reported "3D fix, 0
// satellites used, HDOP 99.99". Per-constellation GSA dimensions were retained
// forever, so a constellation that stopped reporting left a stale 3D behind.
// GGA's quality field is authoritative and now gates the whole thing.
static void test_stale_3d_cleared_by_gga(void) {
    nmea_state_t s;
    nmea_reset(&s);
    assert(nmea_line(&s, "$GNGGA,012952.00,2217.59913,S,16626.35477,E,1,06,1.30,22.3,M,56.6,M,,*66"));
    assert(nmea_line(&s, "$GNGSA,A,3,13,41,08,23,33,,,,,,,,3.36,1.30,3.10,4*0C"));
    assert(s.fix == NMEA_FIX_3D);

    // Signal lost: GGA drops to quality 0 while the stale BeiDou GSA is never
    // contradicted. The fix must go away regardless.
    assert(nmea_line(&s, "$GNGGA,,,,,,0,00,99.99,,,,,,*56"));
    assert(s.fix == NMEA_FIX_NONE);
    assert(s.sats_used == 0);
    assert(!s.have_pos);
}

static void test_gsa_used_per_constellation(void) {
    nmea_state_t s;
    nmea_reset(&s);
    assert(nmea_line(&s, "$GNGGA,012952.00,2217.59913,S,16626.35477,E,1,06,1.30,22.3,M,56.6,M,,*66"));

    // The real bench case: the solution was carried entirely by BeiDou (system
    // id 4, five PRNs) while GPS was visible but contributing nothing.
    assert(nmea_line(&s, "$GNGSA,A,3,13,41,08,23,33,,,,,,,,3.36,1.30,3.10,4*0C"));
    assert(s.used_bds == 5);
    assert(s.used_gps == 0);
    assert(s.fix == NMEA_FIX_3D);

    // A trailing EMPTY GSA from another constellation must not erase that 3D
    // fix — receivers emit one GSA per constellation every cycle.
    assert(nmea_line(&s, "$GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,1*33"));
    assert(s.used_gps == 0);
    assert(s.fix == NMEA_FIX_3D);   // still 3D, from BeiDou

    // GPS joining the solution is counted independently.
    assert(nmea_line(&s, "$GNGSA,A,3,07,30,09,,,,,,,,,,2.10,1.10,1.80,1*06"));
    assert(s.used_gps == 3);
    assert(s.used_bds == 5);
    assert(s.fix == NMEA_FIX_3D);

    // BeiDou dropping out clears only its own count, and the fix survives on GPS.
    assert(nmea_line(&s, "$GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,4*36"));
    assert(s.used_bds == 0);
    assert(s.used_gps == 3);
    assert(s.fix == NMEA_FIX_3D);

    // Everything dropping out, with GGA agreeing, finally clears the fix.
    assert(nmea_line(&s, "$GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,1*33"));
    assert(nmea_line(&s, "$GNGGA,,,,,,0,00,99.99,,,,,,*56"));
    assert(s.fix == NMEA_FIX_NONE);
}

static void test_rmc(void) {
    nmea_state_t s;
    nmea_reset(&s);
    assert(nmea_line(
        &s, "$GNRMC,012656.00,A,2217.59636,S,16626.35420,E,0.724,79.79,250726,,,A,V*28"));
    assert(s.rmc_valid);
    assert(NEAR(s.gs_kt, 0.724));
    assert(NEAR(s.track_deg, 79.79));
    // 2026-07-25T01:26:56Z = 1784942816 s (RMC date field is ddmmyy = 250726)
    assert(s.utc_ms == 1784942816000ULL);

    // A void RMC must never claim validity, and must not zero a known speed.
    nmea_reset(&s);
    assert(nmea_line(&s, "$GNRMC,,V,,,,,,,,,,N,V*37"));
    assert(!s.rmc_valid);
    assert(s.utc_ms == 0);
}

static void test_gsv_per_constellation(void) {
    nmea_state_t s;
    nmea_reset(&s);

    // BeiDou reporting 9 in view across 3 messages — the count is in field 3 and
    // must NOT accumulate as the three parts arrive.
    nmea_line(&s, "$GBGSV,3,1,09,07,41,213,08,08,29,310,21,10,31,216,09,13,16,328,18,1*7F");
    nmea_line(&s, "$GBGSV,3,2,09,23,10,228,20,24,48,095,20,25,50,199,08,33,08,327,26,1*71");
    nmea_line(&s, "$GBGSV,3,3,09,41,08,282,28,1*40");
    assert(s.sats_bds == 9);
    assert(s.sats_view == 9);

    // A second talker adds to the total without disturbing the first.
    nmea_line(&s, "$GPGSV,1,1,02,07,,,27,30,,,22,1*67");
    assert(s.sats_gps == 2);
    assert(s.sats_bds == 9);
    assert(s.sats_view == 11);

    // Galileo reporting zero must clear, not leave a stale count.
    nmea_line(&s, "$GAGSV,1,1,00,0*74");
    assert(s.sats_gal == 0);
    assert(s.sats_view == 11);

    // A re-report replaces rather than adds.
    nmea_line(&s, "$GBGSV,1,1,04,01,50,308,,03,23,285,,04,61,344,,26,09,066,,0*7C");
    assert(s.sats_bds == 4);
    assert(s.sats_view == 6);
}

static void test_robustness(void) {
    nmea_state_t s;
    nmea_reset(&s);
    assert(!nmea_line(&s, ""));
    assert(!nmea_line(&s, "$"));
    assert(!nmea_line(&s, "$GN"));
    assert(!nmea_line(&s, "$GNZZZ,1,2,3*4F"));        // unknown type, no crash
    assert(!nmea_line(&s, "$GNGGA"));                  // no fields at all
    // Truncated mid-field: parses what is there, must not read past the end.
    nmea_line(&s, "$GNGGA,012952.00,2217");
    // Absurdly long input must be rejected outright rather than overflow.
    char big[512];
    memset(big, 'A', sizeof big - 1);
    big[0] = '$';
    big[sizeof big - 1] = 0;
    assert(!nmea_line(&s, big));

    // Empty fields must leave prior state alone, never write zero over it.
    nmea_reset(&s);
    nmea_line(&s, "$GNGGA,012952.00,2217.59913,S,16626.35477,E,1,06,1.30,22.3,M,56.6,M,,*66");
    double keep_lat = s.lat;
    nmea_line(&s, "$GNGGA,,,,,,0,00,99.99,,,,,,*56");
    assert(NEAR(s.lat, keep_lat));   // position retained; have_pos is the gate
    assert(!s.have_pos);
}

int main(void) {
    test_checksum();
    test_gga_no_fix();
    test_gga_3d_fix();
    test_gsa_fix_dimension();
    test_gsa_used_per_constellation();
    test_stale_3d_cleared_by_gga();
    test_rmc();
    test_gsv_per_constellation();
    test_robustness();
    printf("test_nmea OK\n");
    return 0;
}
