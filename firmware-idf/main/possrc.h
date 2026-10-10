// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Position-source policy, kept pure so the decisions that matter most are
// host-testable: which source feeds the EFB, and which identity string wins.
// No ESP-IDF types.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { SRC_NONE, SRC_FEED, SRC_GPS } possrc_t;

// The preferred source wins when it is live; otherwise the other one takes
// over; otherwise nothing is claimed. pref: 1 = GPS, anything else = feed.
possrc_t possrc_choose(bool gps_live, bool feed_live, int pref);

// GNSS quality gate with hysteresis. A cabin-window antenna produces fixes
// that come and go (2D, 3 satellites, HDOP 20); switching the EFB's source on
// each one jumped the position ~1 NM per flip. GNSS counts as live only after
// a 3D fix with HDOP <= GATE_IN_HDOP has held for GATE_IN_MS, and stops being
// live when the fix goes stale or is lost, or HDOP exceeds GATE_OUT_HDOP.
#define GATE_IN_HDOP   5.0
#define GATE_OUT_HDOP  10.0
#define GATE_IN_MS     10000u
typedef struct { bool live; bool good; uint32_t good_since; } gps_gate_t;
// fresh: a fix arrived recently; fix: NMEA_FIX_* value (1 none, 2 2D, 3 3D).
bool possrc_gps_gate(gps_gate_t *g, bool fresh, int fix, double hdop, uint32_t now_ms);

// Per-field identity precedence. The feed is authoritative and wins whenever
// it is non-empty; the manual value fills only what the feed left blank.
// NULL is treated as empty. Always NUL-terminates, never overflows.
void possrc_ident(char *dst, size_t cap, const char *feed, const char *manual);
