// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "geo.h"
#include <math.h>

#define D2R (M_PI / 180.0)
#define R2D (180.0 / M_PI)

double geo_bearing_deg(double lat1, double lon1, double lat2, double lon2) {
    double p1 = lat1 * D2R, p2 = lat2 * D2R, dl = (lon2 - lon1) * D2R;
    double y = sin(dl) * cos(p2);
    double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    double b = atan2(y, x) * R2D;
    return fmod(b + 360.0, 360.0);
}

double geo_dist_nm(double lat1, double lon1, double lat2, double lon2) {
    double p1 = lat1 * D2R, p2 = lat2 * D2R;
    double dp = (lat2 - lat1) * D2R, dl = (lon2 - lon1) * D2R;
    double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    double c = 2 * atan2(sqrt(a), sqrt(1 - a));
    return 3440.065 * c;   // earth radius in nm
}
