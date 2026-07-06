// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include <assert.h>
#include <stdio.h>
#include "tzdb.h"

// 2026-01-15T12:00:00Z and 2026-07-15T12:00:00Z
#define JAN 1768478400u
#define JUL 1784116800u

int main(void) {
    // Nouméa: +11, no DST
    assert(tzdb_offset_min(-22.01, 166.21, JAN) == 660);
    assert(tzdb_offset_min(-22.01, 166.21, JUL) == 660);
    // Paris: CET/CEST
    assert(tzdb_offset_min(48.85, 2.35, JAN) == 60);
    assert(tzdb_offset_min(48.85, 2.35, JUL) == 120);
    // Tokyo: +9, no DST
    assert(tzdb_offset_min(35.68, 139.69, JAN) == 540);
    // Kolkata: +5:30
    assert(tzdb_offset_min(22.57, 88.36, JAN) == 330);
    // Adelaide: +10:30 southern summer, +9:30 winter
    assert(tzdb_offset_min(-34.93, 138.60, JAN) == 630);
    assert(tzdb_offset_min(-34.93, 138.60, JUL) == 570);
    // Sydney: AEDT/AEST
    assert(tzdb_offset_min(-33.87, 151.21, JAN) == 660);
    assert(tzdb_offset_min(-33.87, 151.21, JUL) == 600);
    // Los Angeles: PST/PDT
    assert(tzdb_offset_min(34.05, -118.24, JAN) == -480);
    assert(tzdb_offset_min(34.05, -118.24, JUL) == -420);
    // mid-Pacific ocean: nautical zone
    assert(tzdb_offset_min(0.0, -150.0, JAN) == -600);
    // longitude normalization: 210E == -150
    assert(tzdb_offset_min(0.0, 210.0, JAN) == tzdb_offset_min(0.0, -150.0, JAN));
    // out-of-range latitude falls back to solar estimate
    assert(tzdb_offset_min(91.0, 166.0, JAN) == 11 * 60);

    printf("test_tzdb: PASS\n");
    return 0;
}
