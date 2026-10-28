// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
//   clang -Imain -o /tmp/t host_test/test_batstats.c main/batstats.c main/batgauge.c && /tmp/t
#include "batstats.h"
#include <string.h>

// tenths of a percent gained per hour in the constant-current phase
static int64_t cc_rate10_ph(int cap_mah) { return (int64_t)BATS_CHG_MA * 1000 / cap_mah; }

int batstats_charge_project10(int p10, int secs, int cap_mah)
{
    if (cap_mah <= 0 || p10 < 0) return -1;
    int64_t cc = cc_rate10_ph(cap_mah);                    // tenths per hour
    int64_t cv = cc * BATS_CV_FACTOR10 / 10;
    int64_t t = secs, cvfrom = BATS_CV_FROM * 10;
    int64_t p = p10;
    if (p < cvfrom) {
        int64_t need = (cvfrom - p) * 3600 / cc;           // s to reach the taper
        if (t <= need) return (int)(p + t * cc / 3600);
        p = cvfrom; t -= need;
    }
    p += t * cv / 3600;
    return p > 1000 ? 1000 : (int)p;
}

int batstats_charge_to_full_min(int p10, int cap_mah)
{
    if (cap_mah <= 0 || p10 < 0) return -1;
    int64_t cc = cc_rate10_ph(cap_mah), cv = cc * BATS_CV_FACTOR10 / 10;
    int64_t cvfrom = BATS_CV_FROM * 10, s = 0;
    if (p10 < cvfrom) { s += (cvfrom - p10) * 3600 / cc; p10 = (int)cvfrom; }
    s += (1000 - p10) * 3600 / cv;
    return (int)((s + 59) / 60);
}

void batstats_init(batstats_t *s)
{
    memset(s, 0, sizeof *s);
    s->plug_pct10 = -1;
    s->episode_t0 = -1;
}

// Least-squares slope of the per-minute history, tenths of a % per hour.
static int slope_pph10(const batstats_t *s)
{
    int n = s->ring_n;
    if (n < 6) return -1;                                  // < 5 min of history
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < n; i++) {
        int k = (s->ring_idx - n + i + BATS_RING) % BATS_RING;
        double x = i, y = s->ring[k];
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    double den = n * sxx - sx * sx;
    if (den <= 0) return -1;
    double per_min = (n * sxy - sx * sy) / den;            // tenths per minute
    if (per_min >= -0.02) return -1;                       // not measurably falling yet
    return (int)(-per_min * 60 + 0.5);
}

void batstats_tick(batstats_t *s, int64_t t, batg_state_t st, int cell_mv,
                   int rail_mv, int cap, int remembered, batstats_out_t *o)
{
    memset(o, 0, sizeof *o);
    o->pct10 = o->rate_pph10 = o->draw_ma = o->power_mw = -1;
    o->left_min = o->full_min = o->vbus_mv = o->est_pct = o->to_full_min = -1;
    o->episode_s = -1;

    if (st != s->state && st != BATG_UNKNOWN) {
        // --- episode boundary
        if (st == BATG_ON_USB) {
            s->plug_pct10 = (s->state == BATG_ON_BATTERY) ? s->last_pct10 : -1;
            s->plug_t = t;
        } else if (st == BATG_ON_BATTERY) {
            // A charge that never reached the taper can tell us the capacity:
            // a known current for a known time moved the level by a known amount.
            if (s->state == BATG_ON_USB && s->plug_pct10 >= 0 && t - s->plug_t >= 600) {
                s->learn_pending = true;
                s->learn_from_pct10 = s->plug_pct10;
                s->learn_secs = (int)(t - s->plug_t);
            }
            s->ring_n = s->ring_idx = 0;
            s->ring_t_last = t - 60;
            s->min_mv = 0;
            s->start_pct10 = -1;
        }
        s->state = st;
        s->episode_t0 = t;
    }
    o->state = s->state;
    o->learned_cap_mah = s->learned_cap_mah;
    if (s->episode_t0 >= 0) o->episode_s = (int)(t - s->episode_t0);

    if (s->state == BATG_ON_BATTERY && st == BATG_ON_BATTERY && cell_mv > 0) {
        int p10 = batgauge_pct10_from_mv(cell_mv);
        s->last_pct10 = p10;
        if (s->start_pct10 < 0) s->start_pct10 = p10;
        if (!s->min_mv || cell_mv < s->min_mv) s->min_mv = cell_mv;
        if (t - s->ring_t_last >= 60) {
            s->ring[s->ring_idx] = (int16_t)p10;
            s->ring_idx = (s->ring_idx + 1) % BATS_RING;
            if (s->ring_n < BATS_RING) s->ring_n++;
            s->ring_t_last = t;
        }
        // Capacity learning waits 3 min on battery for the surface charge to
        // fade, and only trusts charges that stayed below the taper.
        if (s->learn_pending && o->episode_s >= 180) {
            s->learn_pending = false;
            int gained = p10 - s->learn_from_pct10;
            if (p10 <= BATS_CV_FROM * 10 && gained >= 100)
                s->learned_cap_mah = (int)((int64_t)BATS_CHG_MA * s->learn_secs * 1000 / 3600 / gained);
            o->learned_cap_mah = s->learned_cap_mah;
        }
        o->pct10 = p10;
        o->used_pct10 = s->start_pct10 - p10 > 0 ? s->start_pct10 - p10 : 0;
        o->min_mv = s->min_mv;
        int r = slope_pph10(s);
        o->rate_live = r > 0;
        if (r <= 0) r = remembered;
        if (r > 0) {
            o->rate_pph10 = r;
            o->left_min = p10 * 60 / r;
            o->full_min = 1000 * 60 / r;
            if (cap > 0) {
                o->draw_ma = (int)((int64_t)r * cap / 1000);
                o->power_mw = o->draw_ma * cell_mv / 1000;
            }
        }
    } else if (s->state == BATG_ON_USB) {
        o->vbus_mv = rail_mv + BATS_SCHOTTKY_MV;
        o->chg_ma = BATS_CHG_MA;
        o->chg_mw = BATS_CHG_MA * 3900 / 1000;              // into a mid-charge cell
        if (remembered > 0) o->full_min = 1000 * 60 / remembered;
        if (remembered > 0 && cap > 0) o->draw_ma = remembered * cap / 1000;
        int e10 = batstats_charge_project10(s->plug_pct10, o->episode_s, cap);
        if (e10 >= 0) {
            o->est_pct = (e10 + 5) / 10;
            o->to_full_min = e10 >= 1000 ? 0 : batstats_charge_to_full_min(e10, cap);
            // Once the model says full, the charger has terminated: no current.
            if (e10 >= 1000) { o->chg_ma = 0; o->chg_mw = 0; }
        }
    }
}
