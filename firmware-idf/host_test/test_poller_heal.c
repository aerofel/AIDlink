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

    // episode clock: brief recoveries inside a degraded window don't reset it
    // (2026-09-19: 45-min degradation with 5 s / 20 s / 78 s recoveries, heal
    // fired at 613 s instead of ~40 min)
    heal_episode_t e = {0};
    assert(heal_episode_ms(&e, 1000) == 0);                 // closed at boot
    heal_episode_fail(&e, 1000);                           // opens at t=1 s
    assert(heal_episode_ms(&e, 1000) == 0);
    assert(heal_episode_ms(&e, 61000) == 60000);
    heal_episode_ok(&e, 62000);                            // 61 s of success
    assert(heal_episode_ms(&e, 62000) == 61000);           // still open
    heal_episode_ok(&e, 1000 + HEAL_RECOVER_MS - 1);       // 1 ms short of held
    assert(heal_episode_ms(&e, 1000 + HEAL_RECOVER_MS - 1) == HEAL_RECOVER_MS - 1);
    heal_episode_fail(&e, 1000 + HEAL_RECOVER_MS - 1);     // relapse: clock keeps t=1 s
    assert(heal_episode_ms(&e, 200000) == 199000);
    heal_episode_ok(&e, 200000);                           // 79 s after the relapse
    assert(heal_episode_ms(&e, 200000) == 199000);
    heal_episode_ok(&e, 1000 + HEAL_RECOVER_MS - 1 + HEAL_RECOVER_MS);   // held: closes
    assert(heal_episode_ms(&e, 1000 + 2 * HEAL_RECOVER_MS) == 0);
    heal_episode_fail(&e, 300000);                         // new episode starts fresh
    assert(heal_episode_ms(&e, 310000) == 10000);
    // ok() on a closed episode is a no-op
    heal_episode_t c = {0};
    heal_episode_ok(&c, 5000); heal_episode_ok(&c, 5000 + 10 * MIN_MS);
    assert(heal_episode_ms(&c, 5000 + 10 * MIN_MS) == 0);
    // wrap-safe: episode opened just before the uint32 ms wrap
    heal_episode_t w = {0};
    heal_episode_fail(&w, 0xFFFFF000u);
    assert(heal_episode_ms(&w, 0x1000u) == 0x2000u);

    // note_fail: only transport failures with the uplink up feed the episode
    heal_episode_t f = {0};
    heal_episode_note_fail(&f, 1000, true, "HTTP 503");     // endpoint answered
    assert(!f.open);
    heal_episode_note_fail(&f, 1000, true, "parse");
    assert(!f.open);
    heal_episode_note_fail(&f, 1000, true, T);              // lockup evidence
    assert(heal_episode_ms(&f, 61000) == 60000);
    heal_episode_note_fail(&f, 61000, true, "HTTP 503");    // doesn't extend...
    assert(f.last_fail_ms == 1000);                         // ...the last failure
    assert(heal_episode_ms(&f, 61000) == 60000);            // ...nor reset the clock
    // uplink down: closes the episode, so a 15-min outage can't be carried
    // into the first failure after reconnect and fire a restart at once
    heal_episode_note_fail(&f, 120000, false, T);
    assert(!f.open);
    heal_episode_note_fail(&f, 15 * MIN_MS, true, T);       // back up, fresh clock
    assert(heal_episode_ms(&f, 15 * MIN_MS + 5000) == 5000);
    assert(!heal_due(heal_episode_ms(&f, 15 * MIN_MS + 5000), 0, true, T));

    // density: one timeout a minute on an otherwise working link never restarts
    heal_episode_t d = {0};
    uint32_t t = 0;
    for (int min = 0; min < 30; min++) {
        heal_episode_note_fail(&d, t, true, T); t += 1000;
        for (int s = 0; s < 59; s++) { heal_episode_ok(&d, t); t += 1000; }
    }
    assert(d.open);                                         // never 2 failure-free minutes
    assert(heal_episode_ms(&d, t) >= 29 * MIN_MS);          // so the clock alone would fire
    assert(!heal_episode_dense(&d));                        // ...but 2 % failing: no restart
    // a real lockup: (almost) everything fails
    heal_episode_t l = {0}; t = 0;
    for (int k = 0; k < 40; k++) { heal_episode_note_fail(&l, t, true, T); t += 15000; }
    heal_episode_ok(&l, t);                                 // one lucky success
    assert(heal_episode_dense(&l));
    // exactly 50 % counts as dense; a fresh episode resets the counts
    heal_episode_t h = {0};
    heal_episode_note_fail(&h, 0, true, T); heal_episode_ok(&h, 1000);
    assert(heal_episode_dense(&h));
    heal_episode_ok(&h, 1000 + HEAL_RECOVER_MS);            // closes
    assert(!h.open && !heal_episode_dense(&h));
    heal_episode_note_fail(&h, 500000, true, T);
    assert(h.fail_n == 1 && h.ok_n == 0);

    puts("test_poller_heal: all OK");
    return 0;
}
