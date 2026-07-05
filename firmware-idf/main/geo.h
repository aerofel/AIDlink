// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Pure great-circle geometry helpers (no ESP-IDF), host-unit-testable.
#pragma once

// Initial great-circle bearing from (lat1,lon1) to (lat2,lon2), degrees true [0,360).
double geo_bearing_deg(double lat1, double lon1, double lat2, double lon2);

// Great-circle distance between two points, nautical miles.
double geo_dist_nm(double lat1, double lon1, double lat2, double lon2);
