// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Application-level liveness supervisor. The task watchdog only watches the
// idle tasks, so a task blocked forever (a lock that never comes back, a
// connect with no timeout) was invisible: the unit stayed up, alive-but-
// useless, for the rest of the flight. Each critical task now beats once per
// loop; when one stays silent past its limit, the supervisor writes the reason
// to the flash log and restarts.
//
// Restart-loop guard: after SUP_MAX_RESTARTS supervisor restarts without
// SUP_HEALTHY_MS of clean uptime in between, it only logs — a fault that
// recurs on every boot must not keep cutting the EFB off every 30 s.
#pragma once
#include <stdint.h>

typedef enum { SUP_ADBP, SUP_POLLER, SUP_DNSFWD, SUP_DISPLAY, SUP_GPS, SUP_N } sup_id_t;

#define SUP_MAX_RESTARTS 3
#define SUP_HEALTHY_MS   (30u * 60u * 1000u)

// Arm supervision of `id`; it must beat at least every limit_ms from now on.
void sup_watch(sup_id_t id, uint32_t limit_ms);
// Called by the task once per loop iteration. Cheap; safe before supervisor_start.
void sup_beat(sup_id_t id);
// Start the checking task. Call once, early.
void supervisor_start(void);
