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

int main(void) {
    test_choose();
    test_identity_precedence();
    printf("test_possrc OK\n");
    return 0;
}
