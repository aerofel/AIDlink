// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Host test for perfdb.c lookups over the generated Offto-derived tables.
//   clang -Imain -o /tmp/t host_test/test_perfdb.c main/perfdb.c main/perfdb_data.c -lm && /tmp/t
#include "perfdb.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

int main(void) {
    assert(perfdb_count() == 31);
    assert(perfdb_get(-1) == 0 && perfdb_get(31) == 0 && perfdb_get(0) != 0);

    // type-code match, case-insensitive
    const perf_ac_t *a = perfdb_find("A339");
    assert(a && a->cruise_kt == 460 && a->ceiling_ft == 41000);
    assert(fabsf(a->climb1_min - 6.5f) < 1e-6f && fabsf(a->climb3_min - 25.0f) < 1e-6f);
    assert(fabsf(a->climb_mach - 0.82f) < 1e-6f);
    assert(strcmp(a->make, "Aircalin") == 0);
    assert(perfdb_find("a339") == a);
    // model-name match reaches the same row
    assert(perfdb_find("A330-900") == a);
    // garbage selects nothing
    assert(perfdb_find("ZZZZ") == 0);
    assert(perfdb_find("") == 0);
    assert(perfdb_find(0) == 0);

    // Aircalin A320neo: real row measured from the 2026-07-14 ACI141 flight
    // (before it existed, the feed's "A20N" matched nothing and the display
    // fell back to the reactive estimator for the whole flight)
    const perf_ac_t *neo = perfdb_find("A20N");
    assert(neo && neo->cruise_kt == 445 && neo->ceiling_ft == 39000);
    assert(strcmp(neo->make, "Aircalin") == 0);
    // operational range generated from airplanes.range (vertical schedule)
    assert(a->max_range_nm == 5500 && neo->max_range_nm == 3500);
    // other neo family codes alias to the nearest ceo row (defense: exact
    // rows always win over the alias table)
    assert(perfdb_find("a19n") == perfdb_find("A319"));
    assert(perfdb_find("A21N") == perfdb_find("A320"));  // no A321 row: nearest
    assert(perfdb_find("A339")->cruise_kt == 460);       // aliases shadow nothing

    // winter Pacific jet: DJF, band 35..40, sector 120..180 -> u ~ +57 m/s
    double u, v;
    perfdb_wind(37.0, 150.0, 1, &u, &v);
    assert(u > 50 && u < 65);
    // same box in July is the JJA season -> a different (much weaker) mean
    double us, vs;
    perfdb_wind(37.0, 150.0, 7, &us, &vs);
    assert(fabs(us - u) > 5);
    // subtropical South Pacific: DJF lat -25..-20, lon -120..-60 sector
    perfdb_wind(-22.5, -100.0, 12, &u, &v);
    assert(fabs(u - 11.0) < 2.0 && fabs(v + 5.0) < 2.0);
    // clamps: poles and antimeridian wrap don't crash / index out
    perfdb_wind(-90.0, 0.0, 1, &u, &v);
    perfdb_wind(90.0, 0.0, 6, &u, &v);
    perfdb_wind(0.0, 185.0, 3, &u, &v);      // wraps to -175 sector
    double u2, v2;
    perfdb_wind(0.0, -175.0, 3, &u2, &v2);
    assert(u == u2 && v == v2);

    printf("test_perfdb: PASS\n");
    return 0;
}
