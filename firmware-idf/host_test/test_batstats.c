// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
//   clang -Imain -o /tmp/t host_test/test_batstats.c main/batstats.c main/batgauge.c && /tmp/t
#include "batstats.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    // --- charging model: 1000 mAh at 580 mA CC
    // 20 % -> 80 % takes 60 % / 58 %/h = 62 min, then 20 % at 23.2 %/h = 52 min
    assert(batstats_charge_to_full_min(200, 1000) == 114);
    assert(batstats_charge_to_full_min(1000, 1000) == 0);
    assert(batstats_charge_project10(200, 3600, 1000) == 780);
    assert(batstats_charge_project10(200, 200 * 60, 1000) == 1000);
    assert(batstats_charge_project10(-1, 60, 1000) == -1);     // unknown start
    assert(batstats_charge_to_full_min(500, 0) == -1);         // unknown capacity

    batstats_t s; batstats_out_t o;
    batstats_init(&s);
    int64_t t = 0;

    // --- cold boot on USB: no level known, so no projection
    for (; t < 30; t++) batstats_tick(&s, t, BATG_ON_USB, 0, 4740, 1000, -1, &o);
    assert(o.state == BATG_ON_USB && o.est_pct == -1 && o.chg_ma == BATS_CHG_MA);
    assert(o.vbus_mv == 5040);

    // --- unplug, discharge 3.95 -> 3.80 V over 40 min (~76 % -> 44 %)
    int mv = 3950;
    for (int k = 0; k < 2400; k++, t++) {
        if (k % 16 == 0) mv--;
        batstats_tick(&s, t, BATG_ON_BATTERY, mv, mv, 1000, -1, &o);
        if (k == 120) assert(o.rate_pph10 == -1);              // 2 min: too early
    }
    assert(o.rate_live && o.rate_pph10 > 0);
    printf("discharge: %d.%d %%/h, draw %d mA, %d mW, %d min left, used %d.%d %%\n",
           o.rate_pph10 / 10, o.rate_pph10 % 10, o.draw_ma, o.power_mw, o.left_min,
           o.used_pct10 / 10, o.used_pct10 % 10);
    assert(o.draw_ma > 300 && o.draw_ma < 1500);
    assert(o.left_min > 30 && o.left_min < 200);
    assert(o.min_mv == mv);
    int level_at_plug = o.pct10;

    // --- plug in for 25 min: projection from the last measured level
    for (int k = 0; k < 1500; k++, t++) batstats_tick(&s, t, BATG_ON_USB, 0, 4740, 1000, 120, &o);
    int expect = batstats_charge_project10(level_at_plug, 1499, 1000);
    assert(o.est_pct == (expect + 5) / 10 && o.to_full_min > 0);
    assert(o.full_min == 500 && o.draw_ma == 120);             // from the remembered rate
    printf("charging: est %d %%, %d min to full\n", o.est_pct, o.to_full_min);

    // --- unplug below the taper: capacity learned after 3 min on battery
    // 25 min at 580 mA = 242 mAh; make the cell gain 24.2 % -> 1000 mAh
    int target10 = level_at_plug + 242;
    int cmv = 3400; while (batgauge_pct10_from_mv(cmv) < target10) cmv++;
    for (int k = 0; k < 200; k++, t++) batstats_tick(&s, t, BATG_ON_BATTERY, cmv, cmv, 0, -1, &o);
    printf("learned capacity: %d mAh\n", o.learned_cap_mah);
    assert(o.learned_cap_mah > 950 && o.learned_cap_mah < 1050);
    assert(o.draw_ma == -1);                                    // capacity unset -> no mA

    puts("batstats: all tests passed");
    return 0;
}
