// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
//   clang -Imain -o /tmp/t host_test/test_poller_heal.c main/poller_heal.c && /tmp/t
#include "poller_heal.h"
#include <assert.h>
#include <stdio.h>

#define MIN_MS (60u * 1000u)

int main(void) {
    // escalation: 10, 20, 40 min; then never
    assert(heal_threshold_ms(0) == 10 * MIN_MS);
    assert(heal_threshold_ms(1) == 20 * MIN_MS);
    assert(heal_threshold_ms(2) == 40 * MIN_MS);
    assert(heal_threshold_ms(HEAL_MAX_RESTARTS) == UINT32_MAX);
    assert(heal_threshold_ms(99) == UINT32_MAX);

    // error classification
    assert( heal_transport_err("ESP_ERR_HTTP_CONNECT"));
    assert( heal_transport_err("ESP_ERR_HTTP_FETCH_HEADER"));
    assert( heal_transport_err("ESP_ERR_HTTP_EAGAIN"));
    assert( heal_transport_err("ESP_FAIL"));
    assert( heal_transport_err("client init (no mem)"));
    assert(!heal_transport_err("HTTP 404"));
    assert(!heal_transport_err("HTTP 302"));
    assert(!heal_transport_err("parse"));
    assert(!heal_transport_err(""));
    assert(!heal_transport_err(NULL));

    // the decision
    const char *T = "ESP_ERR_HTTP_CONNECT";
    assert(!heal_due(9 * MIN_MS + 59000, 0, true,  T));   // not yet
    assert( heal_due(10 * MIN_MS,        0, true,  T));   // exactly the threshold
    assert( heal_due(120 * MIN_MS,       0, true,  T));   // 2 h dead (2026-09-19 case)
    assert(!heal_due(120 * MIN_MS,       0, false, T));   // uplink down: not our bug
    assert(!heal_due(120 * MIN_MS,       0, true,  "HTTP 404"));  // endpoint answered
    assert(!heal_due(120 * MIN_MS,       0, true,  "parse"));
    assert(!heal_due(15 * MIN_MS,        1, true,  T));   // second restart needs 20 min
    assert( heal_due(20 * MIN_MS,        1, true,  T));
    assert(!heal_due(0xFFFFFFF0u,        3, true,  T));   // gave up until power cycle
    assert(!heal_due(0,                  0, true,  T));

    puts("test_poller_heal: all OK");
    return 0;
}
