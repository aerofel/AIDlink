// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Pure config helpers — no ESP-IDF dependencies, host-unit-testable.
#include "config.h"

uint32_t cfg_netmask_from_prefix(uint8_t prefix) {
    if (prefix == 0) return 0u;
    if (prefix >= 32) return 0xFFFFFFFFu;
    return 0xFFFFFFFFu << (32 - prefix);
}
