// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "fixgate.h"
#include <math.h>
#include "geo.h"

bool fixgate_valid(double lat, double lon) {
    if (!isfinite(lat) || !isfinite(lon)) return false;
    if (fabs(lat) > 90.0 || fabs(lon) > 180.0) return false;
    return !(lat == 0.0 && lon == 0.0);
}

// Implied speed from (lat1,lon1) at t1 to (lat2,lon2) at t2. A same-instant
// move counts as 1 s so a glitch inside one poll still reads as a teleport.
static double implied_kt(double lat1, double lon1, uint32_t t1,
                         double lat2, double lon2, uint32_t t2) {
    double dt_h = (double)(uint32_t)(t2 - t1) / 3600000.0;
    if (dt_h < 1.0 / 3600.0) dt_h = 1.0 / 3600.0;
    return geo_dist_nm(lat1, lon1, lat2, lon2) / dt_h;
}

static fg_verdict_t accept(fixgate_t *g, double lat, double lon, uint32_t now, double cur_gs) {
    bool changed = !g->have || lat != g->lat || lon != g->lon;
    if (changed) {
        g->gs_at_change = g->have ? cur_gs : 0;
        g->changed_ms = now;
    }
    g->have = true; g->lat = lat; g->lon = lon;
    g->cand = false;
    return FG_OK;
}

fg_verdict_t fixgate_feed(fixgate_t *g, double lat, double lon, uint32_t now,
                          double cur_gs, double cur_trk, bool have_trk) {
    if (!fixgate_valid(lat, lon)) return FG_BAD;
    if (!g->have) return accept(g, lat, lon, now, cur_gs);

    if (g->cand) {
        if (lat == g->cand_lat && lon == g->cand_lon) {           // candidate repeated
            if ((uint32_t)(now - g->cand_ms) >= FG_HOLD_MAX_MS)
                return accept(g, lat, lon, now, cur_gs);
            return FG_HOLD;
        }
        if (implied_kt(g->cand_lat, g->cand_lon, g->cand_ms, lat, lon, now) <= FG_MAX_KT)
            return accept(g, lat, lon, now, cur_gs);               // the jump was real
        if (implied_kt(g->lat, g->lon, g->changed_ms, lat, lon, now) <= FG_MAX_KT)
            return accept(g, lat, lon, now, cur_gs);               // it was a glitch
        g->cand_lat = lat; g->cand_lon = lon; g->cand_ms = now;    // neither: hold this one
        return FG_HOLD;
    }

    if (lat == g->lat && lon == g->lon) {
        if (g->gs_at_change >= FG_FROZEN_GS_KT &&
            (uint32_t)(now - g->changed_ms) >= FG_FROZEN_MS)
            return FG_FROZEN;
        return FG_OK;                                              // a normal repeat
    }

    bool jump = implied_kt(g->lat, g->lon, g->changed_ms, lat, lon, now) > FG_MAX_KT;
    if (!jump && have_trk && cur_gs >= FG_RESID_MIN_GS) {
        double flown = cur_gs * (double)(uint32_t)(now - g->changed_ms) / 3600000.0;
        double plat, plon;
        geo_dest(g->lat, g->lon, cur_trk, flown, &plat, &plon);
        jump = geo_dist_nm(plat, plon, lat, lon) > FG_RESID_NM + FG_RESID_FRAC * flown;
    }
    if (jump) {
        g->cand = true; g->cand_lat = lat; g->cand_lon = lon; g->cand_ms = now;
        return FG_HOLD;
    }
    return accept(g, lat, lon, now, cur_gs);
}
