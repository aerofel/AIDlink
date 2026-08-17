// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
// Host unit test for the ADBP dead-reckoning hold (spec 2026-08-13):
// tri-state freshness classifier, push-due decision, and DR frame content.
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "adbp_frame.h"

#define STALE   30000u
#define HOLD   300000u

int main(void) {
    // ---------- adbp_classify ----------
    // fresh: valid fix inside the stale window
    assert(adbp_classify(true, 1000, 1000 + STALE - 1, STALE, HOLD) == ADBP_POS_FRESH);
    // at exactly stale_ms the fix is no longer fresh -> DR hold
    assert(adbp_classify(true, 1000, 1000 + STALE, STALE, HOLD) == ADBP_POS_DR);
    // arbiter already marked it invalid: still DR (retained lat/lon carry it)
    assert(adbp_classify(false, 1000, 1000 + STALE + 1000, STALE, HOLD) == ADBP_POS_DR);
    // last ms inside the hold
    assert(adbp_classify(false, 1000, 1000 + STALE + HOLD - 1, STALE, HOLD) == ADBP_POS_DR);
    // hold expired -> NCD
    assert(adbp_classify(false, 1000, 1000 + STALE + HOLD, STALE, HOLD) == ADBP_POS_NCD);
    // dr_hold_ms=0 reproduces today: NCD exactly at stale_ms
    assert(adbp_classify(true, 1000, 1000 + STALE, STALE, 0) == ADBP_POS_NCD);
    assert(adbp_classify(true, 1000, 1000 + STALE - 1, STALE, 0) == ADBP_POS_FRESH);
    // never had a fix (last_fix_ms==0, invalid): NCD even inside the windows
    assert(adbp_classify(false, 0, 5000, STALE, HOLD) == ADBP_POS_NCD);

    // ---------- adbp_push_due ----------
    // on_event: a new fix is always due
    assert(adbp_push_due(true, 42, 41, 10000, 9000, 5000, ADBP_POS_FRESH, ADBP_POS_FRESH));
    // on_event: fix frozen, position still reportable, period elapsed -> fallback push
    assert(adbp_push_due(true, 42, 42, 20000, 14000, 5000, ADBP_POS_DR, ADBP_POS_DR));
    assert(adbp_push_due(true, 42, 42, 20000, 14000, 5000, ADBP_POS_FRESH, ADBP_POS_FRESH));
    // on_event: fix frozen, period NOT elapsed -> not due
    assert(!adbp_push_due(true, 42, 42, 20000, 16000, 5000, ADBP_POS_DR, ADBP_POS_DR));
    // on_event: NCD and no state change -> silent (no fallback past the hold)
    assert(!adbp_push_due(true, 42, 42, 90000, 10000, 5000, ADBP_POS_NCD, ADBP_POS_NCD));
    // any sub: a state transition is due immediately (tells the EFB about DR/NCD)
    assert(adbp_push_due(true, 42, 42, 20000, 19900, 5000, ADBP_POS_NCD, ADBP_POS_DR));
    assert(adbp_push_due(false, 42, 42, 20000, 19900, 5000, ADBP_POS_DR, ADBP_POS_FRESH));
    // periodic: period elapsed -> due in every state (today's behavior)
    assert(adbp_push_due(false, 42, 42, 20000, 15000, 5000, ADBP_POS_NCD, ADBP_POS_NCD));
    assert(!adbp_push_due(false, 42, 42, 20000, 16000, 5000, ADBP_POS_FRESH, ADBP_POS_FRESH));

    // ---------- frame content per state ----------
    pos_state_t p; memset(&p, 0, sizeof p);
    p.valid = true; p.have_track = true; p.lat = -22.27; p.lon = 166.46;
    p.gs_kt = 480; p.track_deg = 270; p.alt_ft = 38000;
    aidlink_cfg_t cfg; memset(&cfg, 0, sizeof cfg);
    cfg.frame_len = 1; cfg.frame_prolog_each = true;

    char names[6][ADBP_MAXNAME];
    strcpy(names[0], "GPSLATP"); strcpy(names[1], "GPSLONGP");
    strcpy(names[2], "GNSS_AVAIL"); strcpy(names[3], "FOM");
    strcpy(names[4], "HDOP"); strcpy(names[5], "GPSTTRKA");
    char out[2048]; bool miss;

    // FRESH: FOM/HDOP are the stock constants (byte-compatible with today)
    adbp_params_block(out, sizeof out, names, 6, &p, &cfg, ADBP_POS_FRESH, 12, 1782000000000ULL, &miss);
    assert(strstr(out, "name=\"GPSLATP\" validity=\"1\""));
    assert(strstr(out, "name=\"FOM\" validity=\"1\" type=\"0\" value=\"8.0\""));
    assert(strstr(out, "name=\"HDOP\" validity=\"1\" type=\"0\" value=\"0.8\""));
    assert(strstr(out, "name=\"GNSS_AVAIL\" validity=\"1\" type=\"6\" value=\"1\""));

    // DR: position still validity=1 with a real value, GNSS_AVAIL stays 1,
    // FOM/HDOP degrade with age (age_s=120 -> FOM 8+2*120=248.0, HDOP 0.8+0.01*120=2.0)
    p.valid = false;   // arbiter has marked it stale; retained fields carry DR
    adbp_params_block(out, sizeof out, names, 6, &p, &cfg, ADBP_POS_DR, 120, 1782000000000ULL, &miss);
    assert(strstr(out, "name=\"GPSLATP\" validity=\"1\""));
    assert(strstr(out, "value=\"-22.270000\""));
    assert(strstr(out, "name=\"GNSS_AVAIL\" validity=\"1\" type=\"6\" value=\"1\""));
    assert(strstr(out, "name=\"FOM\" validity=\"1\" type=\"0\" value=\"248.0\""));
    assert(strstr(out, "name=\"HDOP\" validity=\"1\" type=\"0\" value=\"2.0\""));
    assert(strstr(out, "name=\"GPSTTRKA\" validity=\"1\""));

    // DR degradation caps: very old -> FOM 999.0 / HDOP 9.9
    adbp_params_block(out, sizeof out, names, 6, &p, &cfg, ADBP_POS_DR, 100000, 0, &miss);
    assert(strstr(out, "name=\"FOM\" validity=\"1\" type=\"0\" value=\"999.0\""));
    assert(strstr(out, "name=\"HDOP\" validity=\"1\" type=\"0\" value=\"9.9\""));

    // NCD: today's behavior — bare validity=2, GNSS_AVAIL 0
    adbp_params_block(out, sizeof out, names, 6, &p, &cfg, ADBP_POS_NCD, 400, 0, &miss);
    assert(strstr(out, "name=\"GPSLATP\" validity=\"2\"/>"));
    assert(!strstr(out, "value=\"-22.270000\""));
    assert(strstr(out, "name=\"GNSS_AVAIL\" validity=\"1\" type=\"6\" value=\"0\""));

    printf("test_adbp_hold: PASS\n");
    return 0;
}
