// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
// Host unit test for the pure flash-log sector-ring math (flog_core.c).
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "flog_core.h"

int main(void) {
    // ---------- head sector selection ----------
    flog_hdr_t h[4];
    memset(h, 0xFF, sizeof h);                 // erased flash: no valid magic
    assert(flog_head_sector(h, 4) == -1);      // empty log

    h[0].magic = FLOG_MAGIC; h[0].rsv = 0; h[0].seq = 7;
    assert(flog_head_sector(h, 4) == 0);
    h[2].magic = FLOG_MAGIC; h[2].rsv = 0; h[2].seq = 9;   // later sector wins
    assert(flog_head_sector(h, 4) == 2);
    h[1].magic = 0x1234; h[1].rsv = 0; h[1].seq = 99;      // bad magic never wins
    assert(flog_head_sector(h, 4) == 2);
    h[3].magic = FLOG_MAGIC; h[3].rsv = 0xBEEF; h[3].seq = 99;  // pre-existing flash
    assert(flog_head_sector(h, 4) == 2);                        // garbage never wins

    // ---------- ring advance ----------
    assert(flog_next_sector(2, 4) == 3);
    assert(flog_next_sector(3, 4) == 0);       // wraps

    // ---------- append offset inside a payload ----------
    uint8_t pay[64];
    memset(pay, 0xFF, sizeof pay);
    assert(flog_append_off(pay, sizeof pay) == 0);            // erased -> start
    memcpy(pay, "line one\n", 9);
    assert(flog_append_off(pay, sizeof pay) == 9);            // after the text
    memset(pay, 'x', sizeof pay);
    assert(flog_append_off(pay, sizeof pay) == (int)sizeof pay);  // full

    // ---------- fit check ----------
    assert(flog_fits(0, 64, 64));
    assert(!flog_fits(1, 64, 64));
    assert(flog_fits(60, 4, 64));
    assert(!flog_fits(60, 5, 64));

    printf("test_flog_core: PASS\n");
    return 0;
}
