// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
// Host unit test for the DNS txn-id remap helpers (build with clang, no ESP-IDF).
#include <assert.h>
#include <stdio.h>
#include "dnsfwd.h"

int main(void) {
    uint16_t seq = 0;
    assert(dnsfwd_slot_of(dnsfwd_make_sid(5, &seq)) == 5);

    // slot stable while seq varies -> two ids for the same slot differ
    seq = 0x0FFE;
    uint16_t a = dnsfwd_make_sid(3, &seq);
    uint16_t b = dnsfwd_make_sid(3, &seq);
    assert(dnsfwd_slot_of(a) == 3);
    assert(dnsfwd_slot_of(b) == 3);
    assert(a != b);

    // max slot round-trips
    seq = 0;
    assert(dnsfwd_slot_of(dnsfwd_make_sid(15, &seq)) == 15);
    assert(dnsfwd_slot_of(dnsfwd_make_sid(0, &seq)) == 0);

    printf("test_dnsfwd_remap: PASS (6 assertions)\n");
    return 0;
}
