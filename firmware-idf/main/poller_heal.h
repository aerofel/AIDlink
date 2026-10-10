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
//   - the clock measures a degraded EPISODE, not a bare failure streak: a
//     recovery must hold for HEAL_RECOVER_MS before the clock resets. On
//     2026-09-19 a 45-minute degradation held three recoveries of 5 s, 20 s
//     and 78 s, each of which restarted a plain streak clock, so the restart
//     came at 45 min instead of 10
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define HEAL_BASE_MS       (10u * 60u * 1000u)   // first restart after 10 min dead
#define HEAL_MAX_RESTARTS  3                     // per power cycle without a recovery
#define HEAL_RECOVER_MS    (2u * 60u * 1000u)    // success must hold this long to end an episode

// Degraded-episode clock. Opened by the first failure, closed by a success
// that comes HEAL_RECOVER_MS after the last failure. Zero-init = closed.
typedef struct {
    bool     open;
    uint32_t start_ms;       // first failure of the episode
    uint32_t last_fail_ms;   // most recent failure
    uint32_t fail_n, ok_n;   // polls inside the episode (density, see below)
} heal_episode_t;

// A lockup fails (almost) every poll. A satcom link that times out once a
// minute and succeeds the other 59 times also keeps an episode open — never 2
// failure-free minutes — but restarting it would cut a ~98 % healthy feed.
// Restart only when at least this share of the episode's polls failed.
#define HEAL_MIN_FAIL_PCT  50
bool     heal_episode_dense(const heal_episode_t *e);

void     heal_episode_fail(heal_episode_t *e, uint32_t now_ms);
// What poller.c calls on every failed poll. Only a transport failure while the
// internet probe is green is lockup evidence and extends the episode; a failure
// with the uplink down closes it, because rebooting cannot cure an outage and
// the clock must not carry that outage into the first failure after reconnect.
void     heal_episode_note_fail(heal_episode_t *e, uint32_t now_ms, bool inet_ok,
                                const char *err);
void     heal_episode_ok(heal_episode_t *e, uint32_t now_ms);
// Milliseconds since the episode opened; 0 when closed. Feed this to heal_due().
uint32_t heal_episode_ms(const heal_episode_t *e, uint32_t now_ms);

// Continuous-failure duration required before restart number `n_prior + 1`.
uint32_t heal_threshold_ms(uint32_t n_prior);

// True when `err` (poller's short failure reason) is a transport-level error —
// the endpoint never answered — as opposed to an HTTP status or a parse error.
bool heal_transport_err(const char *err);

// The decision. streak_ms: how long the current degraded episode has lasted
// (heal_episode_ms); n_prior: self-heal restarts already taken since the last recovery
// (or power cycle); inet_ok: the independent internet probe is green now.
bool heal_due(uint32_t streak_ms, uint32_t n_prior, bool inet_ok, const char *err);
