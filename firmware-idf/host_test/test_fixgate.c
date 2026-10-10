// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
//   clang -Imain -o /tmp/t host_test/test_fixgate.c main/fixgate.c main/geo.c -lm && /tmp/t
#include "fixgate.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define S 1000u
// 480 kt eastbound at 21S: degrees of longitude per second
static double dlon_s(void) { return 480.0 / 60.0 / cos(21.0 * M_PI / 180.0) / 3600.0; }

int main(void) {
    // ---- BAD: what atof() makes of junk, and out-of-range values
    assert(!fixgate_valid(0, 0));                  // "", "N/A", missing field
    assert(!fixgate_valid(NAN, 166));
    assert(!fixgate_valid(-22, INFINITY));
    assert(!fixgate_valid(91, 0.5));
    assert(!fixgate_valid(-22, 180.0001));
    assert( fixgate_valid(-22, 180));
    assert( fixgate_valid(0, 166));                 // equator is fine
    assert( fixgate_valid(51.5, 0));                // Greenwich is fine
    fixgate_t g; memset(&g, 0, sizeof g);
    assert(fixgate_feed(&g, 0, 0, 1 * S, 0, 0, false) == FG_BAD);
    assert(!g.have);                                // a BAD fix never becomes the reference

    // ---- cruise with the feed's ~10 s repeats: always OK
    memset(&g, 0, sizeof g);
    double lon = 170.0; uint32_t t = 1 * S;
    assert(fixgate_feed(&g, -21, lon, t, 0, 0, false) == FG_OK);
    for (int i = 1; i <= 60; i++) {                 // 60 s, position changes every 10 s
        t += S;
        if (i % 10 == 0) lon += 10 * dlon_s();
        assert(fixgate_feed(&g, -21, lon, t, 480, 0, false) == FG_OK);
    }

    // ---- FROZEN: cruise, then the feed replays the same position
    uint32_t last_change = t - 0;                   // i=60 changed at t
    for (int i = 1; i < 30; i++)
        assert(fixgate_feed(&g, -21, lon, last_change + i * S, 480, 0, false) == FG_OK);
    assert(fixgate_feed(&g, -21, lon, last_change + 30 * S, 480, 0, false) == FG_FROZEN);
    assert(fixgate_feed(&g, -21, lon, last_change + 600 * S, 0, 0, false) == FG_FROZEN);   // stays frozen
    assert(g.changed_ms == last_change);            // poller backdates the fix to this
    // feed comes back 10 min later, 80 NM on: accepted (480 kt implied)
    double lon2 = lon + 600 * dlon_s();
    assert(fixgate_feed(&g, -21, lon2, last_change + 600 * S, 0, 0, false) == FG_OK);

    // ---- a stop at low speed is the truth, not a frozen feed
    memset(&g, 0, sizeof g);
    assert(fixgate_feed(&g, -22.0, 166.2, 1 * S, 0, 0, false) == FG_OK);
    assert(fixgate_feed(&g, -22.0, 166.2001, 5 * S, 15, 0, false) == FG_OK);    // taxi at 15 kt
    for (int i = 6; i < 600; i++)
        assert(fixgate_feed(&g, -22.0, 166.2001, i * S, 15, 0, false) == FG_OK);
    // ... and so is a rejected takeoff (stops from below FG_FROZEN_GS_KT)
    assert(fixgate_feed(&g, -22.0, 166.21, 700 * S, 150, 0, false) == FG_OK);
    for (int i = 701; i < 800; i++)
        assert(fixgate_feed(&g, -22.0, 166.21, i * S, 150, 0, false) == FG_OK);

    // ---- HOLD: the real 2026-08-16 out-and-back glitch (lat ...2948 -> ...2695 -> ...3018)
    memset(&g, 0, sizeof g);
    assert(fixgate_feed(&g, -21.2948, 167.5, 100 * S, 0, 0, false) == FG_OK);
    assert(fixgate_feed(&g, -21.2695, 167.5, 101 * S, 470, 0, false) == FG_HOLD);   // 1.5 NM in 1 s
    assert(fixgate_feed(&g, -21.2695, 167.5, 102 * S, 470, 0, false) == FG_HOLD);   // repeat proves nothing
    assert(fixgate_feed(&g, -21.3018, 167.5, 103 * S, 470, 0, false) == FG_OK);     // back on track
    assert(!g.cand && g.lat == -21.3018);           // glitch dropped, never published

    // ---- HOLD then confirmed: a genuine jump (e.g. the feed corrected itself)
    memset(&g, 0, sizeof g);
    assert(fixgate_feed(&g, -21.0, 170.0, 100 * S, 0, 0, false) == FG_OK);
    assert(fixgate_feed(&g, -21.0, 170.2, 101 * S, 470, 0, false) == FG_HOLD);      // 11 NM in 1 s
    assert(fixgate_feed(&g, -21.0, 170.2 + 2 * dlon_s(), 103 * S, 470, 0, false) == FG_OK);   // continues from it
    assert(g.lon > 170.19);

    // ---- a candidate the feed keeps repeating is accepted after FG_HOLD_MAX_MS
    memset(&g, 0, sizeof g);
    assert(fixgate_feed(&g, -21.0, 170.0, 100 * S, 0, 0, false) == FG_OK);
    assert(fixgate_feed(&g, -21.0, 171.0, 101 * S, 470, 0, false) == FG_HOLD);
    assert(fixgate_feed(&g, -21.0, 171.0, 101 * S + FG_HOLD_MAX_MS - 1, 470, 0, false) == FG_HOLD);
    assert(fixgate_feed(&g, -21.0, 171.0, 101 * S + FG_HOLD_MAX_MS, 470, 0, false) == FG_OK);

    // ---- a big jump after a long outage is just travel, not a teleport
    memset(&g, 0, sizeof g);
    assert(fixgate_feed(&g, -21.0, 170.0, 100 * S, 480, 0, false) == FG_OK);
    assert(fixgate_feed(&g, -21.0, 175.0, 100 * S + 3600 * S, 480, 0, false) == FG_OK);   // 280 NM in 1 h

    // ---- antimeridian crossing is not a jump
    memset(&g, 0, sizeof g);
    assert(fixgate_feed(&g, -21.0, 179.999, 100 * S, 480, 0, false) == FG_OK);
    assert(fixgate_feed(&g, -21.0, -179.999, 110 * S, 480, 0, false) == FG_OK);

    // ---- uint32 ms wrap
    memset(&g, 0, sizeof g);
    assert(fixgate_feed(&g, -21.0, 170.0, 0xFFFFF000u, 480, 0, false) == FG_OK);
    assert(fixgate_feed(&g, -21.0, 170.0 + 10 * dlon_s(), 0xFFFFF000u + 10 * S, 480, 0, false) == FG_OK);

    // ---- dead-reckoning residual: the glitch the speed test misses
    // (seen on the bench: 1.5 NM sidestep 5 s after the last update = 1195 kt)
    memset(&g, 0, sizeof g);
    double L = 170.0;
    assert(fixgate_feed(&g, -21.0, L, 100 * S, 480, 90, true) == FG_OK);
    assert(fixgate_feed(&g, -21.0 + 0.0253, L + 5 * dlon_s(), 105 * S, 480, 90, true) == FG_HOLD);
    assert(fixgate_feed(&g, -21.0, L + 10 * dlon_s(), 110 * S, 480, 90, true) == FG_OK);
    assert(g.lat == -21.0);                                 // the sidestep never became the reference

    // ---- a standard-rate turn (3 deg/s) at 480 kt with 10 s updates and a
    // track that lags one update behind: never held
    memset(&g, 0, sizeof g);
    double la = -21.0, lo = 170.0, hdg = 90.0, trk_pub = 90.0;
    uint32_t tt = 100 * S;
    assert(fixgate_feed(&g, la, lo, tt, 480, trk_pub, true) == FG_OK);
    for (int k = 0; k < 12; k++) {                          // 120 s, 360 deg
        double pla = la, plo = lo;
        for (int s1 = 0; s1 < 10; s1++) {                   // integrate 1 s steps
            double d = 480.0 / 3600.0;
            la += d / 60.0 * cos(hdg * M_PI / 180);
            lo += d / 60.0 * sin(hdg * M_PI / 180) / cos(la * M_PI / 180);
            hdg += 3.0;
        }
        tt += 10 * S;
        assert(fixgate_feed(&g, la, lo, tt, 480, trk_pub, true) == FG_OK);
        trk_pub = atan2(sin((lo - plo) * M_PI / 180) * cos(la * M_PI / 180),
                        (la - pla) * M_PI / 180) * 180 / M_PI;      // chord bearing, lagging
    }

    // ---- slow / no track: residual test off (taxi wanders, track meaningless)
    memset(&g, 0, sizeof g);
    assert(fixgate_feed(&g, -22.0, 166.2, 1 * S, 0, 0, false) == FG_OK);
    assert(fixgate_feed(&g, -22.0, 166.2004, 5 * S, 15, 270, true) == FG_OK);   // opposite to "track"

    puts("test_fixgate: all OK");
    return 0;
}
