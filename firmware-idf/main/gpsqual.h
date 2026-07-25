// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// GNSS fix quality -> display colour band. Pure and host-tested: this is the
// only thing standing between the operator and a green light on a bad fix.
#pragma once
#include <stdbool.h>
#include "nmea.h"

typedef enum {
    FV_GQ_NONE = 0,  // not applicable (feed is the live source)
    FV_GQ_PURPLE,    // no satellites in view at all
    FV_GQ_RED,       // satellites visible but no fix
    FV_GQ_ORANGE,    // 2D, or 3D that is not trustworthy
    FV_GQ_GREEN,     // 3D and genuinely reliable
    FV_GQ_GREY,      // receiver detected but gone quiet
} fv_gq_t;

// Thresholds: green needs a 3D fix AND HDOP <= 2.0 AND at least 5 satellites
// used. A 3D fix alone is not enough — badly clustered satellites can give a
// 3D solution that is hundreds of metres out while HDOP screams about it.
fv_gq_t fv_gps_quality(nmea_fix_t fix, double hdop, int sats_used,
                       int sats_view, bool silent);
