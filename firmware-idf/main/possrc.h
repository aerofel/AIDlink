// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Position-source policy, kept pure so the decisions that matter most are
// host-testable: which source feeds the EFB, and which identity string wins.
// No ESP-IDF types.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum { SRC_NONE, SRC_FEED, SRC_GPS } possrc_t;

// The preferred source wins when it is live; otherwise the other one takes
// over; otherwise nothing is claimed. pref: 1 = GPS, anything else = feed.
possrc_t possrc_choose(bool gps_live, bool feed_live, int pref);

// Per-field identity precedence. The feed is authoritative and wins whenever
// it is non-empty; the manual value fills only what the feed left blank.
// NULL is treated as empty. Always NUL-terminates, never overflows.
void possrc_ident(char *dst, size_t cap, const char *feed, const char *manual);
