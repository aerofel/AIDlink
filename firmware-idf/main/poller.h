// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Position poller: fetches the configured source (Viasat/Panasonic/custom) over
// HTTP(S), parses the fix, and writes pos.h. Also runs the emulator when enabled.
#pragma once
#include "config.h"

// Start the poller task (periodic fetch + emulator + stale watchdog).
// Takes the live (mutable) config: a received tail number replaces and
// persists the configured aircraft identity.
void poller_start(aidlink_cfg_t *cfg);

// Live poll status for the web /status page. at_ms=0 means "never polled".
void poller_status(bool *ok, uint32_t *at_ms, char *msg, unsigned msgcap);

// Free heap (bytes) sampled at the last poll — surfaced on /status as a
// health gauge for the TLS/HTTP fetch path.
uint32_t poller_last_heap(void);
