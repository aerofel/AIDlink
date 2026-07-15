// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Lookups over the generated performance/wind tables — see perfdb.h.
#include "perfdb.h"
#include <string.h>
#include <strings.h>

int perfdb_count(void) { return PERFDB_NAC; }

const perf_ac_t *perfdb_get(int i) {
    return (i >= 0 && i < PERFDB_NAC) ? &PERFDB_AC[i] : (const perf_ac_t *)0;
}

const perf_ac_t *perfdb_find(const char *type_or_model) {
    if (!type_or_model || !type_or_model[0]) return 0;
    for (int i = 0; i < PERFDB_NAC; i++)
        if (strcasecmp(type_or_model, PERFDB_AC[i].type) == 0)
            return &PERFDB_AC[i];
    for (int i = 0; i < PERFDB_NAC; i++)
        if (strcasecmp(type_or_model, PERFDB_AC[i].model) == 0)
            return &PERFDB_AC[i];
    return 0;
}

void perfdb_wind(double lat, double lon, int month, double *u_ms, double *v_ms) {
    int s = (month == 12 || month == 1 || month == 2) ? 0
          : (month <= 5) ? 1
          : (month <= 8) ? 2 : 3;
    int li = (int)((lat + 90.0) / 5.0);
    if (li < 0) li = 0;
    if (li > 36) li = 36;
    while (lon < -180.0) lon += 360.0;
    while (lon >= 180.0) lon -= 360.0;
    int gi = (int)((lon + 180.0) / 60.0);
    if (gi < 0) gi = 0;
    if (gi > 5) gi = 5;
    if (u_ms) *u_ms = PERFDB_U250[s][li][gi];
    if (v_ms) *v_ms = PERFDB_V250[s][li][gi];
}
