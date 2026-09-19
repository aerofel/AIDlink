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
