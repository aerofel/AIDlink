// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
//   clang -Imain -o /tmp/t host_test/test_batgauge.c main/batgauge.c && /tmp/t
#include "batgauge.h"
#include <string.h>

// --- tuning (rail mV, divider undone) ----------------------------------------
// A 1S cell never exceeds ~4.24 V (TP4065 CV 4.2 V +-1 %); the USB rail sits
// at VBUS minus a Schottky drop, 4.7 V measured on Board 6. Hysteresis keeps a
// droopy port from flapping between the two.
#define USB_ON_MV          4300
#define USB_OFF_MV         4250
#define CELL_MIN_MV        2900    // below: no plausible cell on the rail
// The USB-host flag only rescues a droopy port whose rail sits a little under
// USB_ON_MV. Below this a cell is the likelier source, and the flag can be
// stale: without VBUS monitoring TinyUSB never sees the cable leave.
#define HOST_MIN_MV        4150
// A cell under load shows ADC noise plus the odd Wi-Fi TX dip; the p20..p80
// band ignores the dips. More spread than this = the rail is still moving
// (USB just unplugged, supply settling), so no verdict yet.
#define RIPPLE_MAX_MV      45
#define BATT_WINDOWS       5       // steady windows (s) before trusting a cell
#define USB_WINDOWS        2       // windows (s) before declaring USB
#define PCT_HYST           8       // recovery the draining gauge may step up

static const struct { int16_t mv, pct; } CURVE[] = {
    // 1S LiPo under this board's ~100-150 mA load, coarse but monotonic
    {3400, 0}, {3500, 2}, {3580, 4}, {3640, 7}, {3680, 11}, {3710, 16},
    {3740, 22}, {3760, 28}, {3780, 36}, {3800, 44}, {3830, 52}, {3870, 60},
    {3910, 68}, {3960, 76}, {4020, 84}, {4100, 92}, {4170, 100},
};
#define NCURVE (int)(sizeof CURVE / sizeof CURVE[0])

int batgauge_pct10_from_mv(int mv)
{
    if (mv <= CURVE[0].mv) return 0;
    if (mv >= CURVE[NCURVE - 1].mv) return 1000;
    for (int i = 1; i < NCURVE; i++) {
        if (mv <= CURVE[i].mv) {
            int dm = CURVE[i].mv - CURVE[i - 1].mv, dp = CURVE[i].pct - CURVE[i - 1].pct;
            return CURVE[i - 1].pct * 10 + ((mv - CURVE[i - 1].mv) * dp * 10 + dm / 2) / dm;
        }
    }
    return 1000;
}

int batgauge_pct_from_mv(int mv) { return (batgauge_pct10_from_mv(mv) + 5) / 10; }

void batgauge_init(batgauge_t *g)
{
    memset(g, 0, sizeof *g);
    g->pct = -1;
}

static void sort16(int16_t *a, int n)
{
    for (int i = 1; i < n; i++) {
        int16_t v = a[i]; int j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
}

void batgauge_sample(batgauge_t *g, int mv, bool usb_host)
{
    g->ring[g->idx] = (int16_t)mv;
    g->idx = (g->idx + 1) % BATG_WIN;
    if (g->n < BATG_WIN) g->n++;
    if (++g->tick % 10 || g->n < BATG_WIN) return;   // evaluate once a second

    int16_t s[BATG_WIN];
    memcpy(s, g->ring, sizeof s);
    sort16(s, BATG_WIN);
    int med = s[BATG_WIN / 2];
    g->med_mv = med;
    g->ripple_mv = s[BATG_WIN * 4 / 5] - s[BATG_WIN / 5];

    bool usb = med >= (g->state == BATG_ON_USB ? USB_OFF_MV : USB_ON_MV) ||
               (usb_host && med >= HOST_MIN_MV);
    bool cell = !usb && med >= CELL_MIN_MV && g->ripple_mv <= RIPPLE_MAX_MV;
    g->usb_run  = usb  ? g->usb_run + 1  : 0;
    g->batt_run = cell ? g->batt_run + 1 : 0;

    if (g->usb_run >= USB_WINDOWS || (usb && g->state == BATG_ON_USB)) {
        g->state = BATG_ON_USB;
        g->v_q4 = 0;
        g->pct = -1;
        return;
    }
    if (g->batt_run < BATT_WINDOWS) {
        // Leaving a state needs evidence too: a single odd window must not
        // blank a gauge that has been reading steadily.
        if (g->state == BATG_ON_BATTERY && g->batt_run == 0 && !usb &&
            med >= CELL_MIN_MV) return;
        if (!(g->state == BATG_ON_BATTERY && cell)) g->state = BATG_UNKNOWN;
        if (g->state == BATG_UNKNOWN) { g->v_q4 = 0; g->pct = -1; }
        return;
    }

    // Steady on battery: ~6 s exponential smoothing of the window median.
    // A new episode starts from the raw reading; within one, the gauge only
    // steps down, except for a large recovery (load drop, surface charge).
    bool fresh = (g->state != BATG_ON_BATTERY) || !g->v_q4;
    g->state = BATG_ON_BATTERY;
    if (fresh) g->v_q4 = med * 16;
    else       g->v_q4 += (med * 16 - g->v_q4) / 6;
    int raw = batgauge_pct_from_mv(g->v_q4 / 16);
    if (fresh || g->pct < 0 || raw < g->pct || raw > g->pct + PCT_HYST) g->pct = raw;
}
