// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Aircraft performance profiles + seasonal wind climatology, generated from
// the Offto project's database (see tools/gen_perfdb.py). Pure — no ESP-IDF
// dependencies, host-testable.
#pragma once
#include <stdint.h>

typedef struct {
    const char *make, *model;
    char     type[5];        // ICAO type code — the lookup / persistence key
    uint16_t cruise_kt;      // cruise TAS, knots (constant, no Mach modeling)
    float    climb1_min;     // surface -> FL100 (avg GS 280 kt)
    float    climb2_min;     // FL100 -> FL200   (avg GS 380 kt)
    float    climb3_min;     // FL200 -> TOC     (avg GS climb_mach x 593.7 kt)
    float    climb_mach;
    uint32_t ceiling_ft;     // cruise altitude MSL; descent dist = ceiling/300
} perf_ac_t;

int perfdb_count(void);
const perf_ac_t *perfdb_get(int i);

// Exact ICAO type-code match first, then exact model-name match (both
// case-insensitive). NULL when nothing matches — feed garbage selects nothing.
const perf_ac_t *perfdb_find(const char *type_or_model);

// Seasonal 250 hPa (jet-level) climatological wind at a position.
// month 1..12 -> DJF/MAM/JJA/SON. u east+ / v north+ in m/s.
void perfdb_wind(double lat, double lon, int month, double *u_ms, double *v_ms);

// generated tables (perfdb_data.c); season index 0=DJF 1=MAM 2=JJA 3=SON,
// lat band i covers [-90+5i, -85+5i), lon sector j covers [-180+60j, -120+60j)
extern const int PERFDB_NAC;
extern const perf_ac_t PERFDB_AC[];
extern const int8_t PERFDB_U250[4][37][6], PERFDB_V250[4][37][6];
extern const int8_t PERFDB_U300[4][37][6], PERFDB_V300[4][37][6];
