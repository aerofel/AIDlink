// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "poller_heal.h"
#include <string.h>

uint32_t heal_threshold_ms(uint32_t n_prior) {
    if (n_prior >= HEAL_MAX_RESTARTS) return UINT32_MAX;
    return HEAL_BASE_MS << n_prior;
}

bool heal_transport_err(const char *err) {
    if (!err || !*err) return false;
    if (strncmp(err, "HTTP ", 5) == 0) return false;   // the endpoint answered
    if (strcmp(err, "parse") == 0) return false;       // so did the body
    return true;
}

bool heal_due(uint32_t streak_ms, uint32_t n_prior, bool inet_ok, const char *err) {
    if (!inet_ok) return false;
    if (!heal_transport_err(err)) return false;
    uint32_t thr = heal_threshold_ms(n_prior);
    return thr != UINT32_MAX && streak_ms >= thr;
}

void heal_episode_fail(heal_episode_t *e, uint32_t now_ms) {
    if (!e->open) { e->open = true; e->start_ms = now_ms; e->fail_n = e->ok_n = 0; }
    e->last_fail_ms = now_ms;
    e->fail_n++;
}

bool heal_episode_dense(const heal_episode_t *e) {
    uint32_t n = e->fail_n + e->ok_n;
    return e->open && n && e->fail_n * 100u >= HEAL_MIN_FAIL_PCT * n;
}

void heal_episode_note_fail(heal_episode_t *e, uint32_t now_ms, bool inet_ok,
                            const char *err) {
    if (!inet_ok) { e->open = false; return; }
    if (heal_transport_err(err)) heal_episode_fail(e, now_ms);
}

void heal_episode_ok(heal_episode_t *e, uint32_t now_ms) {
    if (e->open) e->ok_n++;
    if (e->open && now_ms - e->last_fail_ms >= HEAL_RECOVER_MS) e->open = false;
}

uint32_t heal_episode_ms(const heal_episode_t *e, uint32_t now_ms) {
    return e->open ? now_ms - e->start_ms : 0;
}
