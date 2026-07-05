// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
// Host unit test for cfg_netmask_from_prefix (build with clang, no ESP-IDF).
#include <assert.h>
#include <stdio.h>
#include "config.h"

int main(void) {
    assert(cfg_netmask_from_prefix(26) == 0xFFFFFFC0u);
    assert(cfg_netmask_from_prefix(29) == 0xFFFFFFF8u);
    assert(cfg_netmask_from_prefix(24) == 0xFFFFFF00u);
    assert(cfg_netmask_from_prefix(32) == 0xFFFFFFFFu);
    assert(cfg_netmask_from_prefix(0)  == 0x00000000u);
    printf("test_config_subnet: PASS (5 assertions)\n");
    return 0;
}
