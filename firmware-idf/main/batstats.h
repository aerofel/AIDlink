// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Power / autonomy figures for the portal's battery panel. Pure logic, fed
// once a second by battery.c, host-tested in host_test/test_batstats.c.
//
// The board has no current sensor and cannot see the cell while on USB (see
// batgauge.h), so every figure is one of:
//   measured  - cell voltage / percentage, only while running on battery;
//   derived   - discharge rate from the falling percentage, and mA / W from
//               that rate x the configured capacity;
//   modelled  - charging: the TP4065 is programmed for 580 mA constant
//               current (R13), taper assumed above 80 %; the level is
//               projected from the last percentage measured before plug-in.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "batgauge.h"

#define BATS_CHG_MA      580     // TP4065 PROG resistor R13 (LilyGO schematic)
#define BATS_CV_FROM     80      // % where constant-voltage taper begins (model)
#define BATS_CV_FACTOR10 4       // mean taper current, tenths of the CC current
#define BATS_SCHOTTKY_MV 300     // VBUS -> rail drop (1N5819 at board load)
#define BATS_RING        30      // discharge history, one point per minute

typedef struct {
    // inputs remembered between ticks
    batg_state_t state;
    int64_t episode_t0;           // s, start of the current battery/USB episode
    // on battery
    int     start_pct10, last_pct10, min_mv;
    int16_t ring[BATS_RING];      // pct10 once a minute
    int     ring_n, ring_idx;
    int64_t ring_t_last;
    // charging
    int     plug_pct10;           // last measured level before plug-in, -1 unknown
    int64_t plug_t;
    bool    learn_pending;        // a charge episode may yield a capacity estimate
    int     learn_from_pct10, learn_secs;
    int     learned_cap_mah;      // 0 = none yet
} batstats_t;

typedef struct {
    int state;                    // batg_state_t
    int episode_s;                // seconds in the current state, -1 unknown
    // on battery
    int pct10;                    // measured level, -1 when not on battery
    int used_pct10;               // since unplug
    int min_mv;
    int rate_pph10;               // discharge %/h x10, -1 unknown
    bool rate_live;               // measured this episode (else remembered)
    int draw_ma, power_mw;        // -1 unknown (needs capacity)
    int left_min;                 // time to empty, -1 unknown
    int full_min;                 // autonomy from 100 %, -1 unknown
    // on USB
    int vbus_mv;                  // estimated USB supply, -1 when on battery
    int chg_ma, chg_mw;           // nominal charge current / power into the cell
    int est_pct;                  // projected level, -1 unknown
    int to_full_min;              // -1 unknown
    // learning
    int learned_cap_mah;          // 0 = none
} batstats_out_t;

void batstats_init(batstats_t *s);
// Once a second. cell_mv is the smoothed cell voltage (0 when not on
// battery), rail_mv the raw rail reading, cap_mah 0 when unknown,
// remembered_pph10 a rate saved from an earlier episode (-1 none).
void batstats_tick(batstats_t *s, int64_t t_s, batg_state_t st, int cell_mv,
                   int rail_mv, int cap_mah, int remembered_pph10, batstats_out_t *o);

// Charging model, exposed for tests: projected level (tenths) after `secs`
// from `from_pct10`, and minutes from `pct10` to full.
int batstats_charge_project10(int from_pct10, int secs, int cap_mah);
int batstats_charge_to_full_min(int pct10, int cap_mah);
