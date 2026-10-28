// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
//   clang -Imain -o /tmp/t host_test/test_batgauge.c main/batgauge.c && /tmp/t
//
// Rail levels are the ones read off Board 6 on 2026-10-28: 4.74 V on USB with
// a cell fitted (the rail is VBUS minus a Schottky, see batgauge.h).
#include "batgauge.h"
#include <assert.h>
#include <stdio.h>

static unsigned rng = 12345;
static int noise(int amp) {               // uniform +-amp mV
    rng = rng * 1103515245u + 12345u;
    return (int)((rng >> 16) % (2 * amp + 1)) - amp;
}
typedef int (*gen_t)(int t_ds);
static void run(batgauge_t *g, int secs, gen_t f, bool usb, int *t) {
    for (int i = 0; i < secs * 10; i++, (*t)++) batgauge_sample(g, f(*t), usb);
}
static int g_level = 3900;
static int cell(int t)      { (void)t; return g_level + noise(6); }
static int cell_wifi(int t) { return g_level + noise(6) - ((t % 13) == 0 ? 120 : 0); }
static int usb_rail(int t)  { (void)t; return 4740 + noise(5); }
static int nothing(int t)   { (void)t; return 40 + noise(20); }

int main(void) {
    batgauge_t g; int t = 0;

    // curve: endpoints, monotonic, tenths agree with whole percent
    assert(batgauge_pct_from_mv(3300) == 0 && batgauge_pct_from_mv(4200) == 100);
    for (int mv = 3300, last = 0; mv <= 4250; mv += 5) {
        int p = batgauge_pct10_from_mv(mv); assert(p >= last); last = p;
        assert(batgauge_pct_from_mv(mv) == (p + 5) / 10);
    }
    assert(batgauge_pct_from_mv(3740) == 22 && batgauge_pct_from_mv(3680) == 11);

    // USB, battery or not: the rail says USB and never yields a percentage
    batgauge_init(&g); t = 0;
    run(&g, 60, usb_rail, false, &t);
    assert(g.state == BATG_ON_USB && g.pct == -1);
    printf("USB rail %d mV -> on USB\n", g.med_mv);

    // a droopy port below the threshold still counts as USB with a host
    batgauge_init(&g); t = 0; g_level = 4200;
    run(&g, 10, cell, true, &t);
    assert(g.state == BATG_ON_USB);

    // Unplugged on battery while TinyUSB still reports "mounted": with no VBUS
    // monitoring the stack never sees the detach (esp_tinyusb README, self-
    // powered devices), so a stuck host flag must not hide a running cell.
    batgauge_init(&g); t = 0;
    run(&g, 10, usb_rail, true, &t);
    assert(g.state == BATG_ON_USB);
    g_level = 3950;
    run(&g, 15, cell_wifi, true, &t);
    assert(g.state == BATG_ON_BATTERY && g.pct > 0);
    printf("stuck USB-host flag, cell at %d mV: on battery, %d%%\n", batgauge_cell_mv(&g), g.pct);

    // nothing on the rail
    batgauge_init(&g); t = 0;
    run(&g, 30, nothing, false, &t);
    assert(g.state == BATG_UNKNOWN);

    // on battery with Wi-Fi dips: measured after the steady-window run
    batgauge_init(&g); t = 0; g_level = 3900;
    run(&g, 5, cell_wifi, false, &t);
    assert(g.state == BATG_UNKNOWN);
    run(&g, 10, cell_wifi, false, &t);
    assert(g.state == BATG_ON_BATTERY && g.pct >= 62 && g.pct <= 70);
    printf("on battery 3.90 V: %d%%, ripple %d mV\n", g.pct, g.ripple_mv);

    // draining: only ever steps down
    int last = g.pct;
    for (int k = 0; k < 60; k++) {
        g_level -= 3; run(&g, 10, cell_wifi, false, &t);
        assert(g.state == BATG_ON_BATTERY && g.pct <= last); last = g.pct;
    }
    assert(last < 20);
    printf("drained to %d mV: %d%%\n", batgauge_cell_mv(&g), last);

    // plug in: USB within a couple of seconds, percentage withdrawn
    run(&g, 4, usb_rail, false, &t);
    assert(g.state == BATG_ON_USB && g.pct == -1);

    // unplug a charged cell: transition, then a fresh measured level
    g_level = 4120;
    run(&g, 3, cell_wifi, false, &t);
    assert(g.state != BATG_ON_BATTERY);          // window still holds USB samples
    run(&g, 12, cell_wifi, false, &t);
    assert(g.state == BATG_ON_BATTERY && g.pct >= 90 && g.pct < 100);
    printf("unplugged at %d mV: %d%%\n", batgauge_cell_mv(&g), g.pct);

    // one disturbed window does not blank a steady gauge
    for (int i = 0; i < 10; i++, t++) batgauge_sample(&g, (i & 1) ? 4300 : 3900, false);
    assert(g.state == BATG_ON_BATTERY);

    puts("batgauge: all tests passed");
    return 0;
}
