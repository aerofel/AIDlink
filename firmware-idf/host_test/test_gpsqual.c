// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
//   clang -Imain -o /tmp/t host_test/test_gpsqual.c main/gpsqual.c && /tmp/t
#include "gpsqual.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    // The real fix from the bench session: 3D, HDOP 1.30, 6 sats.
    assert(fv_gps_quality(NMEA_FIX_3D, 1.30, 6, 12, false) == FV_GQ_GREEN);
    // Exactly on both thresholds still counts as reliable.
    assert(fv_gps_quality(NMEA_FIX_3D, 2.00, 5, 9, false) == FV_GQ_GREEN);

    // A 3D fix alone is NOT enough — this is the case the colour exists for.
    assert(fv_gps_quality(NMEA_FIX_3D, 2.01, 8, 9, false) == FV_GQ_ORANGE);
    assert(fv_gps_quality(NMEA_FIX_3D, 8.00, 8, 9, false) == FV_GQ_ORANGE);
    assert(fv_gps_quality(NMEA_FIX_3D, 1.00, 4, 9, false) == FV_GQ_ORANGE);
    assert(fv_gps_quality(NMEA_FIX_2D, 1.00, 8, 9, false) == FV_GQ_ORANGE);
    // HDOP of zero means "not reported"; it must not read as perfect.
    assert(fv_gps_quality(NMEA_FIX_3D, 0.0, 8, 9, false) == FV_GQ_ORANGE);

    // Tracking something but not solving: the state the unit sat in indoors.
    assert(fv_gps_quality(NMEA_FIX_NONE, 99.99, 0, 1, false) == FV_GQ_RED);
    assert(fv_gps_quality(NMEA_FIX_NONE, 99.99, 0, 12, false) == FV_GQ_RED);

    // Nothing in view at all — antenna unplugged, or looking at the ground.
    assert(fv_gps_quality(NMEA_FIX_NONE, 99.99, 0, 0, false) == FV_GQ_PURPLE);

    // Silence outranks every other signal, however healthy it last looked.
    assert(fv_gps_quality(NMEA_FIX_3D, 1.00, 8, 9, true) == FV_GQ_GREY);
    assert(fv_gps_quality(NMEA_FIX_NONE, 99.99, 0, 0, true) == FV_GQ_GREY);

    printf("test_gpsqual OK\n");
    return 0;
}
