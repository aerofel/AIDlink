// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "tzdb.h"
#include <math.h>

int tzdb_offset_min(double lat, double lon, uint32_t utc_epoch_s) {
    // normalize longitude to [-180, 180)
    while (lon >= 180.0) lon -= 360.0;
    while (lon < -180.0) lon += 360.0;
    if (lat > 90.0 || lat < -90.0)
        return (int)lround(lon / 15.0) * 60;     // garbage in -> solar estimate

    int r = (int)floor(lat) + 90;  if (r > 179) r = 179; if (r < 0) r = 0;
    int c = (int)floor(lon) + 180; if (c > 359) c = 359; if (c < 0) c = 0;
    uint8_t zi = TZDB_GRID[r * 360 + c];
    if (zi >= TZDB_NZONES)
        return (int)lround(lon / 15.0) * 60;

    const tzdb_zone_t *z = &TZDB_ZONES[zi];
    int off = z->t[0].off;
    for (int i = 1; i < z->n; i++)
        if (utc_epoch_s >= z->t[i].at) off = z->t[i].off;
    return off;
}
