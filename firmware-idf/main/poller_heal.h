// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Last-resort self-heal for a dead position feed. Pure decision logic (host-
// testable); poller.c owns the counters and performs the restart.
//
// Every poller lockup recorded so far (2026-07-24, 2026-09-04 x2, 2026-09-19)
// shared one property: the uplink was fine, only the device's own TLS client
// was dead, and a reboot cured it instantly while nothing else ever did. So
// after a long continuous transport-level failure WITH the internet probe
// green, restarting is the one remedy with a 100% record. Guards:
//   - only transport errors count (an "HTTP 404" or a parse failure means the
//     endpoint answered; rebooting cannot help and would loop on a bad config)
//   - the wait doubles after each restart that did not bring the feed back,
//     and after HEAL_MAX_RESTARTS the unit stops trying until a power cycle
//   - a recovery resets the escalation, so a recurring lockup keeps getting
//     the fast 10-minute treatment instead of a longer and longer wait
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define HEAL_BASE_MS       (10u * 60u * 1000u)   // first restart after 10 min dead
#define HEAL_MAX_RESTARTS  3                     // per power cycle without a recovery

// Continuous-failure duration required before restart number `n_prior + 1`.
uint32_t heal_threshold_ms(uint32_t n_prior);

// True when `err` (poller's short failure reason) is a transport-level error —
// the endpoint never answered — as opposed to an HTTP status or a parse error.
bool heal_transport_err(const char *err);

// The decision. streak_ms: how long the feed has failed without a single
// success; n_prior: self-heal restarts already taken since the last recovery
// (or power cycle); inet_ok: the independent internet probe is green now.
bool heal_due(uint32_t streak_ms, uint32_t n_prior, bool inet_ok, const char *err);
