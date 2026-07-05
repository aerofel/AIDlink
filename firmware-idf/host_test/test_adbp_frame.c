// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
// Host unit test for the pure ADBP frame builders.
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "adbp_frame.h"

static int count_sub(const char *h, const char *n) {
    int c = 0; for (const char *p = h; (p = strstr(p, n)); p += strlen(n)) c++; return c;
}

int main(void) {
    char out[2048];

    // --- parse_params ignores <method name> and reads <parameter name> ---
    const char *req = "<method name=\"getAvionicParameters\">"
                      "<parameter name=\"LATITUDE\"/><parameter name=\"LONGITUDE\"/>"
                      "<parameter name=\"GROUNDSPEED\"/></method>";
    char names[ADBP_MAXPARAMS][ADBP_MAXNAME];
    int n = adbp_parse_params(req, names, ADBP_MAXPARAMS);
    assert(n == 3);
    assert(strcmp(names[0], "LATITUDE") == 0);
    assert(strcmp(names[2], "GROUNDSPEED") == 0);

    // --- tag_num ---
    assert(adbp_tag_num("<publishport>51001</publishport>", "publishport", 0) == 51001);
    assert(adbp_tag_num("<refreshperiod>250</refreshperiod>", "refreshperiod", 5000) == 250);
    assert(adbp_tag_num("no tags here", "publishport", 42) == 42);

    // --- params_block with a valid fix ---
    pos_state_t p; memset(&p, 0, sizeof p);
    p.valid = true; p.have_track = true; p.lat = 47.5; p.lon = -122.3;
    p.gs_kt = 450; p.track_deg = 90; p.alt_ft = 35000;
    strcpy(p.tail, "F-ONEO"); strcpy(p.orig, "NOU"); strcpy(p.dest, "SYD");
    aidlink_cfg_t cfg; memset(&cfg, 0, sizeof cfg);
    cfg.frame_len = 1; cfg.frame_prolog_each = true; strcpy(cfg.ac_tail, "TEST01");
    bool miss = false;
    adbp_params_block(out, sizeof out, names, n, &p, &cfg, true, 1782000000000ULL, &miss);
    assert(strstr(out, "<parameters>") && strstr(out, "</parameters>"));
    assert(strstr(out, "value=\"47.500000\""));    // LAT
    assert(strstr(out, "value=\"-122.300000\""));  // LON
    assert(strstr(out, "value=\"450.0\""));        // GS
    assert(!miss);

    // unknown param -> NCD + miss
    char un[1][ADBP_MAXNAME]; strcpy(un[0], "TOTALLYBOGUS");
    adbp_params_block(out, sizeof out, un, 1, &p, &cfg, true, 0, &miss);
    assert(strstr(out, "validity=\"2\""));
    assert(miss);

    // --- wrap_push length="" is self-consistent (frame_len=1: element length) ---
    const char *body = "<parameters><parameter name=\"LAT\" validity=\"1\" type=\"0\" value=\"1.0\"/></parameters>";
    int len = adbp_wrap_push(out, sizeof out, "publishAvionicParameters", body, true, &cfg);
    // extract the advertised length and compare to actual <method ...>...</method> element length
    char *m = strstr(out, "<method ");
    assert(m);
    int adv = 0; sscanf(strstr(out, "length=\"") + 8, "%d", &adv);
    int elem = (int)(len - (m - out));   // from <method to end (prolog precedes it)
    assert(adv == elem);   // length attribute equals the method element's byte length

    // frame_len=2 omits length attribute
    cfg.frame_len = 2;
    adbp_wrap_push(out, sizeof out, "publishAvionicParameters", body, true, &cfg);
    assert(!strstr(out, "length="));
    assert(strstr(out, "<method name=\"publishAvionicParameters\">"));

    // prolog present when requested, absent when not
    cfg.frame_len = 1;
    adbp_wrap_push(out, sizeof out, "publishAvionicParameters", body, false, &cfg);
    assert(count_sub(out, "<?xml") == 0);
    adbp_wrap_push(out, sizeof out, "publishAvionicParameters", body, true, &cfg);
    assert(count_sub(out, "<?xml") == 1);

    printf("test_adbp_frame: PASS\n");
    return 0;
}
