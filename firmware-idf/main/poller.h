// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Position poller: fetches the configured source (Viasat/Panasonic/custom) over
// HTTP(S), parses the fix, and writes pos.h. Also runs the emulator when enabled.
#pragma once
#include "config.h"

// Start the poller task (periodic fetch + emulator + stale watchdog).
void poller_start(const aidlink_cfg_t *cfg);

// Live poll status for the web /status page. at_ms=0 means "never polled".
void poller_status(bool *ok, uint32_t *at_ms, char *msg, unsigned msgcap);
