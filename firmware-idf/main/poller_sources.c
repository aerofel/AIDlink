// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Source JSON parsers (Viasat nested-"value" + Panasonic flat td_id_*). Pure —
// only cJSON + libc — so they can be host-unit-tested against sample payloads.
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

// --- ISO8601 "YYYY-MM-DDThh:mm:ss" -> epoch ms UTC (0 if unparseable) ---
static long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
static uint64_t iso_to_epoch_ms(const char *s) {
    int Y, Mo, D, h, mi, se;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &Mo, &D, &h, &mi, &se) != 6) return 0;
    long days = days_from_civil(Y, Mo, D);
    return ((uint64_t)days * 86400 + h * 3600 + mi * 60 + se) * 1000ULL;
}

// Viasat: field may be a nested object {"value":"X"} or a direct scalar. Return
// the scalar cJSON node (descending into ".value" when present), or NULL.
static cJSON *viasat_scalar(const cJSON *root, const char *key) {
    cJSON *f = cJSON_GetObjectItemCaseSensitive((cJSON *)root, key);
    if (!f) return NULL;
    if (cJSON_IsObject(f)) return cJSON_GetObjectItemCaseSensitive(f, "value");
    return f;
}
// numeric value (accepts quoted-number strings), returns false if absent
static bool viasat_num(const cJSON *root, const char *key, double *out) {
    cJSON *v = viasat_scalar(root, key);
    if (!v) return false;
    if (cJSON_IsNumber(v)) { *out = v->valuedouble; return true; }
    if (cJSON_IsString(v) && v->valuestring) { *out = atof(v->valuestring); return true; }
    return false;
}
static bool viasat_str(const cJSON *root, const char *key, char *out, size_t cap) {
    cJSON *v = viasat_scalar(root, key);
    if (!v || !cJSON_IsString(v) || !v->valuestring) return false;
    strncpy(out, v->valuestring, cap - 1); out[cap - 1] = 0; return true;
}

bool poller_parse_viasat(const char *json, double *lat, double *lon, double *alt,
                         double *gs, double *track, bool *have_track, uint64_t *utc_ms,
                         char *flight, char *tail, char *orig, char *dest, char *actype) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    bool ok = false;
    double la, lo;
    if (viasat_num(root, "latitude", &la) && viasat_num(root, "longitude", &lo)) {
        *lat = la; *lon = lo; ok = true;
        double t;
        *alt = viasat_num(root, "altitude", &t) ? t : 0;
        *gs = viasat_num(root, "groundSpeed", &t) ? t : -1;   // -1 => derive
        *track = 0; *have_track = false;                       // Viasat has no track
        viasat_str(root, "flightNumber", flight, 16);
        viasat_str(root, "tail_number", tail, 12);
        viasat_str(root, "originCode", orig, 8);
        viasat_str(root, "destinationCode", dest, 8);
        // best-effort: neither pinned live capture carries a type field; this
        // key follows the feed's camelCase style in case a variant serves one
        viasat_str(root, "aircraftType", actype, 8);
        char ts[40];
        *utc_ms = viasat_str(root, "current_time", ts, sizeof ts) ? iso_to_epoch_ms(ts) : 0;
    }
    cJSON_Delete(root);
    return ok;
}

// Panasonic lat/lon: 8-digit string, deg*1000, >=80000000 means negative.
static double pan_ll(const char *s) {
    long v = atol(s);
    return v >= 80000000L ? -((v - 80000000L) / 1000.0) : (v / 1000.0);
}
static const char *pan_str(const cJSON *root, const char *key) {
    cJSON *f = cJSON_GetObjectItemCaseSensitive((cJSON *)root, key);
    return (f && cJSON_IsString(f)) ? f->valuestring : NULL;
}

bool poller_parse_panasonic(const char *json, double *lat, double *lon, double *alt,
                            double *gs, double *track, bool *have_track, uint64_t *utc_ms,
                            char *flight, char *tail, char *orig, char *dest, char *actype) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    bool ok = false;
    const char *slat = pan_str(root, "td_id_fltdata_present_position_latitude");
    const char *slon = pan_str(root, "td_id_fltdata_present_position_longitude");
    if (slat && slon) {
        *lat = pan_ll(slat); *lon = pan_ll(slon); ok = true;
        const char *s;
        *alt = (s = pan_str(root, "td_id_fltdata_altitude")) ? atof(s) : 0;
        *gs = (s = pan_str(root, "td_id_fltdata_ground_speed")) ? atof(s) : -1;
        if ((s = pan_str(root, "td_id_fltdata_true_heading"))) { *track = atof(s); *have_track = true; }
        else { *track = 0; *have_track = false; }
        if ((s = pan_str(root, "td_id_fltdata_flight_number"))) strncpy(flight, s, 15);
        if ((s = pan_str(root, "td_id_airframe_tail_number"))) strncpy(tail, s, 11);
        if ((s = pan_str(root, "td_id_airframe_model"))) strncpy(actype, s, 7);   // best-effort key
        if ((s = pan_str(root, "td_id_fltdata_departure_id"))) strncpy(orig, s, 7);
        if ((s = pan_str(root, "td_id_fltdata_destination_id"))) strncpy(dest, s, 7);
        *utc_ms = 0;   // Panasonic has no time field
    }
    cJSON_Delete(root);
    return ok;
}
