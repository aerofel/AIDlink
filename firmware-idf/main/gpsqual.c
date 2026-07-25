// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "gpsqual.h"

#define HDOP_RELIABLE 2.0
#define SATS_RELIABLE 5

fv_gq_t fv_gps_quality(nmea_fix_t fix, double hdop, int sats_used,
                       int sats_view, bool silent) {
    // A receiver that stopped talking tells us nothing, whatever it last said.
    if (silent) return FV_GQ_GREY;

    if (fix == NMEA_FIX_NONE)
        return sats_view > 0 ? FV_GQ_RED : FV_GQ_PURPLE;

    if (fix == NMEA_FIX_3D && hdop > 0 && hdop <= HDOP_RELIABLE &&
        sats_used >= SATS_RELIABLE)
        return FV_GQ_GREEN;

    return FV_GQ_ORANGE;
}
