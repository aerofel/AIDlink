// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "flog_core.h"

int flog_head_sector(const flog_hdr_t *hdrs, int n) {
    int best = -1; uint32_t best_seq = 0;
    for (int i = 0; i < n; i++) {
        // rsv must read 0 too: the partition is never bulk-erased at first
        // boot, so pre-existing flash garbage must not fake a valid header.
        if (hdrs[i].magic != FLOG_MAGIC || hdrs[i].rsv != 0) continue;
        if (best < 0 || hdrs[i].seq > best_seq) { best = i; best_seq = hdrs[i].seq; }
    }
    return best;
}

int flog_next_sector(int cur, int n) { return (cur + 1) % n; }

int flog_append_off(const uint8_t *payload, int paylen) {
    for (int i = 0; i < paylen; i++) if (payload[i] == 0xFF) return i;
    return paylen;
}

bool flog_fits(int off, int len, int paylen) { return off + len <= paylen; }
