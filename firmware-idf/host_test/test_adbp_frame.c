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
    adbp_params_block(out, sizeof out, names, n, &p, &cfg, ADBP_POS_FRESH, 0, 1782000000000ULL, &miss);
    assert(strstr(out, "<parameters>") && strstr(out, "</parameters>"));
    assert(strstr(out, "value=\"47.500000\""));    // LAT
    assert(strstr(out, "value=\"-122.300000\""));  // LON
    assert(strstr(out, "value=\"450.0\""));        // GS
    assert(!miss);

    // unknown param -> NCD + miss
    char un[1][ADBP_MAXNAME]; strcpy(un[0], "TOTALLYBOGUS");
    adbp_params_block(out, sizeof out, un, 1, &p, &cfg, ADBP_POS_FRESH, 0, 0, &miss);
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

    // names AIDlink answers vs names it can only answer NCD (logged per subscribe)
    assert(adbp_param_known("GPSLAT") && adbp_param_known("gpslong") && adbp_param_known("THDG"));
    assert(!adbp_param_known("WINDSPEED") && !adbp_param_known(""));

    // FliteDeck FOMAX names (subscribe log 2026-10-10): every *PF is the FINE
    // half of a coarse+fine pair and must read 0 — the coarse already carries
    // full precision. HYBLATPF used to return the whole latitude (coarse+fine
    // = 97 deg). ALTNDEST read as altitude and DESTQNH as the destination code.
    {
        pos_state_t q = {0}; aidlink_cfg_t qc = {0};
        q.valid = true; q.lat = 48.664612; q.lon = 13.435761; q.have_track = true;
        q.gs_kt = 450; q.alt_ft = 39972; strcpy(q.dest, "LFPG");
        char nm[8][ADBP_MAXNAME] = { "HYBLATPF", "HYBLONGPF", "GPSLATPF", "GPSLONGPF",
                                     "HYBLATP", "FMSLONGP", "ALTNDEST", "DESTQNH" };
        char out[2048]; bool miss;
        adbp_params_block(out, sizeof out, nm, 8, &q, &qc, ADBP_POS_FRESH, 0, 1, &miss);
        assert(strstr(out, "name=\"HYBLATPF\" validity=\"1\" type=\"0\" value=\"0.000000\""));
        assert(strstr(out, "name=\"HYBLONGPF\" validity=\"1\" type=\"0\" value=\"0.000000\""));
        assert(strstr(out, "name=\"GPSLATPF\" validity=\"1\" type=\"0\" value=\"0.000000\""));
        assert(strstr(out, "name=\"GPSLONGPF\" validity=\"1\" type=\"0\" value=\"0.000000\""));
        assert(strstr(out, "name=\"HYBLATP\" validity=\"1\" type=\"0\" value=\"48.664612\""));
        assert(strstr(out, "name=\"FMSLONGP\" validity=\"1\" type=\"0\" value=\"13.435761\""));
        assert(strstr(out, "<parameter name=\"ALTNDEST\" validity=\"2\" type=\""));
        assert(strstr(out, "<parameter name=\"DESTQNH\" validity=\"2\" type=\""));
        assert(!adbp_param_known("ALTNDEST") && !adbp_param_known("DESTQNH"));
    }

    // --- SystemDate / SystemTime / UTCTIME from the frame's UTC stamp (FliteDeck 5.3) ---
    {
        char tn[3][ADBP_MAXNAME] = {"SystemDate", "SystemTime", "UTCTIME"};
        bool m2;
        // 2026-11-11 04:08:30.250 UTC, independent of the position state
        adbp_params_block(out, sizeof out, tn, 3, &p, &cfg, ADBP_POS_NCD, 0, 1794370110250ULL, &m2);
        assert(!m2);
        assert(strstr(out, "<parameter name=\"SystemDate\" validity=\"1\" type=\"8\" value=\"2026/11/11\""));
        assert(strstr(out, "<parameter name=\"SystemTime\" validity=\"1\" type=\"9\" value=\"04:08:30\""));
        assert(strstr(out, "<parameter name=\"UTCTIME\" validity=\"1\" type=\"9\" value=\"04:08:30\""));
        // no real clock (0, or adbp.c's synthetic floor): NCD, never a made-up date
        adbp_params_block(out, sizeof out, tn, 3, &p, &cfg, ADBP_POS_FRESH, 0, 0, &m2);
        assert(strstr(out, "<parameter name=\"SystemDate\" validity=\"2\" type=\""));
        adbp_params_block(out, sizeof out, tn, 3, &p, &cfg, ADBP_POS_FRESH, 0, ADBP_SYNTH_STAMP_MS + 500, &m2);
        assert(strstr(out, "<parameter name=\"SystemTime\" validity=\"2\" type=\""));
        assert(adbp_param_known("SystemDate") && adbp_param_known("SystemTime"));
    }

    // --- FliteDeck 5.3 (AIC core 3.0.110) rejects a WHOLE frame with
    // "Parameter.value is None" if any <parameter> lacks value=. Every element,
    // NCD included, must carry type, value and time. ---
    {
        char an[6][ADBP_MAXNAME] = {"TAT", "ACTYP", "SystemDate", "SystemTime", "DESTQNH", "GPSLATP"};
        bool m3;
        adbp_params_block(out, sizeof out, an, 6, &p, &cfg, ADBP_POS_NCD, 0, 0, &m3);
        int elems = 0;
        for (const char *e = strstr(out, "<parameter "); e; e = strstr(e + 1, "<parameter ")) {
            const char *end = strstr(e, "/>");
            assert(end);
            char one[256]; size_t L = (size_t)(end - e); assert(L < sizeof one);
            memcpy(one, e, L); one[L] = 0;
            assert(strstr(one, " type=\"") && strstr(one, " value=\"") && strstr(one, " time=\""));
            assert(!strstr(one, "value=\"\""));          // never an empty value either
            elems++;
        }
        assert(elems == 6);
        assert(strstr(out, "<parameter name=\"TAT\" validity=\"2\" type=\"0\" value=\"0\""));
        assert(strstr(out, "<parameter name=\"ACTYP\" validity=\"2\" type=\"7\" value=\"-\""));
        assert(strstr(out, "<parameter name=\"SystemDate\" validity=\"2\" type=\"8\" value=\"1970/01/01\""));
        assert(strstr(out, "<parameter name=\"SystemTime\" validity=\"2\" type=\"9\" value=\"00:00:00\""));
    }

    printf("test_adbp_frame: PASS\n");
    return 0;
}
