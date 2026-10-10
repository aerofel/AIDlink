// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "possrc.h"
#include <string.h>

possrc_t possrc_choose(bool gps_live, bool feed_live, int pref) {
    // Only pref == 1 means GPS; any other value is treated as "feed", so a
    // corrupt NVS read degrades to today's behaviour rather than to no fix.
    bool want_gps = (pref == 1);
    if (want_gps) {
        if (gps_live)  return SRC_GPS;
        if (feed_live) return SRC_FEED;
    } else {
        if (feed_live) return SRC_FEED;
        if (gps_live)  return SRC_GPS;
    }
    return SRC_NONE;
}

void possrc_ident(char *dst, size_t cap, const char *feed, const char *manual) {
    if (!dst || cap == 0) return;
    const char *src = (feed && feed[0]) ? feed : manual;
    if (!src) { dst[0] = 0; return; }
    size_t n = 0;
    while (n < cap - 1 && src[n]) { dst[n] = src[n]; n++; }
    dst[n] = 0;
}

bool possrc_gps_gate(gps_gate_t *g, bool fresh, int fix, double hdop, uint32_t now) {
    bool good = fresh && fix == 3 && hdop > 0 && hdop <= GATE_IN_HDOP;
    if (good && !g->good) g->good_since = now;
    g->good = good;
    if (g->live) {
        if (!fresh || fix < 2 || !(hdop > 0 && hdop <= GATE_OUT_HDOP)) g->live = false;
    } else if (good && (uint32_t)(now - g->good_since) >= GATE_IN_MS) {
        g->live = true;
    }
    return g->live;
}
