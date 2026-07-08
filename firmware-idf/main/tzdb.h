// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Embedded world timezone lookup: a 1° lat/lon grid of zone indices plus each
// zone's UTC-offset transition table (covers DST and odd offsets like +5:30 or
// Chatham's +12:45). Data in tzdb_data.c is GENERATED from the IANA tz database
// (tools/gen_tzdb.py; see LEARNING.md 2026-07-06); coverage is ~2 years
// from generation — regenerate alongside firmware updates. Pure C, host-tested.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define TZDB_MAXTRANS 8

typedef struct {
    uint8_t n;                                   // valid entries in t[]
    struct { uint32_t at; int16_t off; } t[TZDB_MAXTRANS];  // offset (min) from epoch `at`
} tzdb_zone_t;

extern const uint8_t    TZDB_NZONES;
extern const tzdb_zone_t TZDB_ZONES[];
extern const uint8_t    TZDB_GRID[180 * 360];    // [lat -90..89][lon -180..179]

// UTC offset in minutes at (lat, lon) for the given UTC epoch second.
// Falls back to the solar estimate (round(lon/15)*60) outside grid/table range.
int tzdb_offset_min(double lat, double lon, uint32_t utc_epoch_s);
