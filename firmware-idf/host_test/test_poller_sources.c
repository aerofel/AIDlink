// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
// Host test for the source JSON parsers (compiled against real cJSON source).
#include <assert.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

bool poller_parse_viasat(const char *json, double *lat, double *lon, double *alt,
                         double *gs, double *track, bool *have_track, uint64_t *utc_ms,
                         char *flight, char *tail, char *orig, char *dest);
bool poller_parse_panasonic(const char *json, double *lat, double *lon, double *alt,
                            double *gs, double *track, bool *have_track, uint64_t *utc_ms,
                            char *flight, char *tail, char *orig, char *dest);

int main(void) {
    double lat, lon, alt, gs, track; bool ht; uint64_t utc;
    char flight[16] = "", tail[12] = "", orig[8] = "", dest[8] = "";

    // --- Viasat nested {"value":...} shape (matches examples/flight_info.json) ---
    const char *v = "{\"latitude\":{\"value\":\"1.359\"},\"longitude\":{\"value\":\"103.99\"},"
                    "\"altitude\":{\"value\":\"37000\"},\"groundSpeed\":{\"value\":\"0\"},"
                    "\"flightNumber\":{\"value\":\"TEST01\"},\"tail_number\":{\"value\":\"F-ONEO\"},"
                    "\"originCode\":{\"value\":\"WSSS\"},\"destinationCode\":{\"value\":\"WMKK\"},"
                    "\"current_time\":{\"value\":\"2026-01-01T00:00:00Z\"}}";
    assert(poller_parse_viasat(v, &lat, &lon, &alt, &gs, &track, &ht, &utc, flight, tail, orig, dest));
    assert(fabs(lat - 1.359) < 1e-6);
    assert(fabs(lon - 103.99) < 1e-6);
    assert(fabs(alt - 37000) < 1e-6);
    assert(fabs(gs - 0.0) < 1e-6);      // groundSpeed 0 present
    assert(!ht);                         // Viasat never provides track
    assert(strcmp(flight, "TEST01") == 0);
    assert(strcmp(tail, "F-ONEO") == 0);
    assert(strcmp(orig, "WSSS") == 0 && strcmp(dest, "WMKK") == 0);
    // 2026-01-01T00:00:00Z = 1767225600 s
    assert(utc == 1767225600000ULL);

    // missing lat/lon -> fail
    assert(!poller_parse_viasat("{\"altitude\":{\"value\":\"100\"}}", &lat, &lon, &alt, &gs, &track, &ht, &utc, flight, tail, orig, dest));

    // --- Real Viasat /ac/flight/info capture (2026-02-27, F-ONEA NWWW->NFFN):
    // full attr/updated_at/value wrapping, "+0000" time suffix, NO groundSpeed
    // field at all (gs must come back -1 = derive-from-fixes).
    const char *vr = "{\"altitude\":{\"attr\":{\"display_name\":\"Altitude\"},\"updated_at\":1772220174,\"value\":\"36968\"},"
                     "\"city_pair\":{\"attr\":{},\"updated_at\":1772230034,\"value\":\"NWWW NFFN\"},"
                     "\"current_time\":{\"attr\":{},\"updated_at\":1772220174,\"value\":\"2026-02-27T22:14:48+0000\"},"
                     "\"destinationCode\":{\"attr\":{},\"updated_at\":1772220174,\"value\":\"NFFN\"},"
                     "\"flightNumber\":{\"attr\":{},\"updated_at\":1772220174,\"value\":\"ACI330\"},"
                     "\"latitude\":{\"attr\":{},\"updated_at\":1772220174,\"value\":\"-18.5564\"},"
                     "\"longitude\":{\"attr\":{},\"updated_at\":1772220174,\"value\":\"172.12\"},"
                     "\"originCode\":{\"attr\":{},\"updated_at\":1772220174,\"value\":\"NWWW\"},"
                     "\"tail_number\":{\"attr\":{},\"updated_at\":1772220174,\"value\":\"F-ONEA\"}}";
    assert(poller_parse_viasat(vr, &lat, &lon, &alt, &gs, &track, &ht, &utc, flight, tail, orig, dest));
    assert(fabs(lat - (-18.5564)) < 1e-6 && fabs(lon - 172.12) < 1e-6);
    assert(fabs(alt - 36968) < 1e-6);
    assert(gs == -1 && !ht);                       // no groundSpeed/track in feed
    assert(strcmp(flight, "ACI330") == 0 && strcmp(tail, "F-ONEA") == 0);
    assert(strcmp(orig, "NWWW") == 0 && strcmp(dest, "NFFN") == 0);
    assert(utc == 1772230488000ULL);               // 2026-02-27T22:14:48Z

    // --- Panasonic flat td_id_* with deg*1000 sign encoding ---
    memset(flight, 0, sizeof flight); memset(tail, 0, sizeof tail);
    const char *p = "{\"td_id_fltdata_present_position_latitude\":\"00007980\","   //  7.980
                    "\"td_id_fltdata_present_position_longitude\":\"80005000\","    // -(5.000)
                    "\"td_id_fltdata_altitude\":\"39000\",\"td_id_fltdata_ground_speed\":\"470\","
                    "\"td_id_fltdata_true_heading\":\"120\",\"td_id_fltdata_flight_number\":\"SB800\","
                    "\"td_id_airframe_tail_number\":\"F-ONEA\","
                    "\"td_id_fltdata_departure_id\":\"NOU\",\"td_id_fltdata_destination_id\":\"SYD\"}";
    assert(poller_parse_panasonic(p, &lat, &lon, &alt, &gs, &track, &ht, &utc, flight, tail, orig, dest));
    assert(fabs(lat - 7.980) < 1e-6);
    assert(fabs(lon - (-5.000)) < 1e-6);   // 80005000 -> -5.000
    assert(fabs(gs - 470) < 1e-6);
    assert(ht && fabs(track - 120) < 1e-6);
    assert(strcmp(flight, "SB800") == 0 && strcmp(tail, "F-ONEA") == 0);

    printf("test_poller_sources: PASS\n");
    return 0;
}
