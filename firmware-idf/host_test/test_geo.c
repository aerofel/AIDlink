// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "geo.h"

int main(void) {
    // due north
    double b = geo_bearing_deg(0, 0, 1, 0);
    assert(fabs(b - 0.0) < 0.5 || fabs(b - 360.0) < 0.5);
    // due east (near equator)
    b = geo_bearing_deg(0, 0, 0, 1);
    assert(fabs(b - 90.0) < 0.5);
    // due south
    b = geo_bearing_deg(1, 0, 0, 0);
    assert(fabs(b - 180.0) < 0.5);
    // due west
    b = geo_bearing_deg(0, 1, 0, 0);
    assert(fabs(b - 270.0) < 0.5);

    // ~60 nm per degree of latitude
    double d = geo_dist_nm(0, 0, 1, 0);
    assert(fabs(d - 60.0) < 1.0);

    printf("test_geo: PASS\n");
    return 0;
}
