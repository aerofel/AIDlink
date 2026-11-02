// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Internal-SRAM watch for the flash log (2026-11-02). The 10-minute "[mem]
// imin=" line showed the internal heap's low-water collapsing to a few KB (even
// 112 B) in most long sessions while the 60 s samples looked healthy, i.e. a
// transient hog -- but never WHEN it happened, so it could not be lined up
// with AP joins, poller reconnects or DHCP failures. This samples once a second
// and logs, timestamped:
//   - each new internal low-water (below MW_WARN_BELOW, then every MW_STEP
//     further down), for both the malloc pool and ALL internal RAM (the latter
//     includes the 32 KB reserve that Wi-Fi/DMA allocations fall back to);
//   - every allocation that actually FAILED (heap failed-alloc hook).
// It also hosts the mbedTLS allocator (PSRAM first), see memwatch.c.
#pragma once
#include <stdint.h>

// Register the failed-allocation hook. Call once, early (after log init).
void memwatch_init(void);

// Sample the heap and log anything new. Call ~1 Hz from a task with a few KB
// of stack (it calls logln).
void memwatch_tick(void);

// The poller calls this on every new HTTP(S) connection, so a low-water line
// can be lined up with TLS handshakes (the suspected internal-SRAM hog).
void memwatch_note_connect(void);
uint32_t memwatch_connects(void);
