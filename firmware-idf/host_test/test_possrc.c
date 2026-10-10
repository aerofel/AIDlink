// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
//   clang -Imain -o /tmp/t host_test/test_possrc.c main/possrc.c && /tmp/t
#include "possrc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define PREF_FEED 0
#define PREF_GPS  1

static void test_choose(void) {
    // Preference wins whenever it is live...
    assert(possrc_choose(true,  true,  PREF_GPS)  == SRC_GPS);
    assert(possrc_choose(true,  false, PREF_GPS)  == SRC_GPS);
    assert(possrc_choose(true,  true,  PREF_FEED) == SRC_FEED);
    assert(possrc_choose(false, true,  PREF_FEED) == SRC_FEED);

    // ...and the other source takes over when it is not. This is the whole
    // point: neither a GNSS dropout nor a feed stall may blank Jeppesen.
    assert(possrc_choose(false, true,  PREF_GPS)  == SRC_FEED);
    assert(possrc_choose(true,  false, PREF_FEED) == SRC_GPS);

    // Nothing live means nothing claimed — never a stale position.
    assert(possrc_choose(false, false, PREF_GPS)  == SRC_NONE);
    assert(possrc_choose(false, false, PREF_FEED) == SRC_NONE);

    // An out-of-range preference must not fall through to "no position".
    assert(possrc_choose(true, true, 42) == SRC_FEED);
}

static void test_identity_precedence(void) {
    char d[12];

    // The feed is authoritative: it wins whenever it has something to say.
    possrc_ident(d, sizeof d, "F-ONET", "F-XXXX");
    assert(!strcmp(d, "F-ONET"));

    // The manual value fills only what the feed left empty.
    possrc_ident(d, sizeof d, "", "F-XXXX");
    assert(!strcmp(d, "F-XXXX"));

    // Both empty stays empty — never a placeholder that could reach the EFB.
    possrc_ident(d, sizeof d, "", "");
    assert(d[0] == 0);

    // NULL is treated as empty, not dereferenced.
    possrc_ident(d, sizeof d, NULL, "F-XXXX");
    assert(!strcmp(d, "F-XXXX"));
    possrc_ident(d, sizeof d, NULL, NULL);
    assert(d[0] == 0);

    // Overlong input truncates and stays NUL-terminated rather than overflowing.
    possrc_ident(d, sizeof d, "", "ABCDEFGHIJKLMNOPQRST");
    assert(strlen(d) == sizeof d - 1);
    assert(!strcmp(d, "ABCDEFGHIJK"));
}

// GNSS quality gate: in after 10 s of good 3D, out on loss or HDOP > 10.
static void test_gps_gate(void) {
    gps_gate_t g = {0};
    uint32_t t = 1000;
    assert(!possrc_gps_gate(&g, true, 2, 1.0, t));              // 2D never opens it
    assert(!possrc_gps_gate(&g, true, 3, 20.0, t += 1000));     // window antenna: bad HDOP
    assert(!possrc_gps_gate(&g, true, 3, 1.2, t += 1000));      // good from here...
    assert(!possrc_gps_gate(&g, true, 3, 1.2, t += 9999));      // ...9.999 s: not yet
    assert( possrc_gps_gate(&g, true, 3, 1.2, t += 1));         // 10 s: open
    assert( possrc_gps_gate(&g, true, 2, 8.0, t += 1000));      // 2D, HDOP 8: stays (hysteresis)
    assert(!possrc_gps_gate(&g, true, 3, 12.0, t += 1000));     // HDOP > 10: closed
    assert(!possrc_gps_gate(&g, true, 3, 1.0, t += 1000));      // good again: must re-earn 10 s
    assert(!possrc_gps_gate(&g, true, 3, 1.0, t += 5000));
    assert(!possrc_gps_gate(&g, true, 2, 1.0, t += 1000));      // a 2D blip restarts the clock
    assert(!possrc_gps_gate(&g, true, 3, 1.0, t += 1000));
    assert(!possrc_gps_gate(&g, true, 3, 1.0, t += 9000));
    assert( possrc_gps_gate(&g, true, 3, 1.0, t += 1000));
    assert(!possrc_gps_gate(&g, false, 3, 1.0, t += 1000));     // stale: closed at once
    assert(!possrc_gps_gate(&g, true, 1, 0.0, t += 1000));      // no fix
}

int main(void) {
    test_choose();
    test_identity_precedence();
    test_gps_gate();
    printf("test_possrc OK\n");
    return 0;
}
