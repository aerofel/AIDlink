// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Pure DNS txn-id remap helpers — no ESP-IDF dependencies, host-unit-testable.
#include "dnsfwd.h"

uint16_t dnsfwd_make_sid(int slot, uint16_t *seq) {
    return (uint16_t)(((slot & 0x0F) << 12) | ((*seq)++ & 0x0FFF));
}

int dnsfwd_slot_of(uint16_t sid) {
    return (sid >> 12) & 0x0F;
}
