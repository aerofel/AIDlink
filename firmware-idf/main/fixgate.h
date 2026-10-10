// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Gate between the parsed feed position and what AIDlink publishes. Pure logic
// (host-tested); poller.c owns the state and acts on the verdict.
//
// Three ways a feed can hand us a position that must not reach the EFB as valid:
//
//  BAD     not a position at all: NaN/inf, out of range, or exactly 0,0 — what
//          atof() makes of "", "N/A" or a missing field.
//  HOLD    a jump no aircraft can fly (> FG_MAX_KT from the last accepted
//          position), or — when moving with a known track — a position more
//          than FG_RESID_NM + FG_RESID_FRAC x (distance flown) away from where
//          the last fix, speed and track put the aircraft. The speed test alone
//          misses a glitch that lands a few seconds after the last update (a
//          1.5 NM sidestep 5 s later reads as 1195 kt). Held, not published,
//          until the next DIFFERENT position says which one was wrong: consistent with the jump -> it was real;
//          consistent with the old track -> it was a glitch and is dropped.
//          The feed repeats each position for ~10 s between avionics updates,
//          so a repeat of the candidate proves nothing; a candidate repeated
//          for FG_HOLD_MAX_MS is accepted (the feed means it).
//  FROZEN  the position has not changed for FG_FROZEN_MS although the speed at
//          its last change was >= FG_FROZEN_GS_KT. No aircraft stops that fast
//          (a rejected takeoff starts below it), so the feed is replaying a
//          cached fix: stop treating it as fresh. Below that speed — taxi, the
//          gate, a real stop — a still position is the truth and stays fresh.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define FG_MAX_KT         1200.0
#define FG_RESID_NM       1.0       // a standard-rate turn strays ~0.35 NM in 10 s
#define FG_RESID_FRAC     0.25
#define FG_RESID_MIN_GS   50.0
#define FG_HOLD_MAX_MS    20000u
#define FG_FROZEN_MS      30000u    // == DERIVE_STILL_MS: decide before derive zeroes GS
#define FG_FROZEN_GS_KT   200.0

typedef enum { FG_OK = 0, FG_BAD, FG_HOLD, FG_FROZEN } fg_verdict_t;

typedef struct {
    bool     have;                 // a position has been accepted
    double   lat, lon;             // last accepted position
    uint32_t changed_ms;           // when the accepted position last changed
    double   gs_at_change;         // published GS just before that change
    bool     cand;                 // an unconfirmed jump is being held
    double   cand_lat, cand_lon;
    uint32_t cand_ms;              // when the candidate first appeared
} fixgate_t;

// Judge one parsed position. cur_gs / cur_trk / have_trk = kinematics currently
// published for the feed. On FG_OK the position becomes the new reference.
fg_verdict_t fixgate_feed(fixgate_t *g, double lat, double lon, uint32_t now_ms,
                          double cur_gs, double cur_trk, bool have_trk);

// True when lat/lon can be a real position (finite, in range, not 0,0).
bool fixgate_valid(double lat, double lon);
