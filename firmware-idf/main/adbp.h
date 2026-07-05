// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// ADBP (ARINC 834 AID Data Basic Protocol) server: a TCP command socket on
// cfg.adbp_port that answers getAvionicParameters / subscribeAvionicParameters /
// unSubscribe, plus a periodic push of subscribed parameters to each client's
// advertised publishport. Reads ownship state from pos.h.
#pragma once
#include "config.h"

// Start the ADBP server task (command accept loop + periodic push).
void adbp_start(const aidlink_cfg_t *cfg);

// True if we're actively feeding position to an EFB: at least one active
// subscription we've pushed to within the last few seconds.
bool adbp_feeding(void);
