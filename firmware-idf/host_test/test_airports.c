// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "airports.h"
#include "geo.h"

int main(void) {
    double la, lo;

    // IATA and ICAO resolve to the same airport, case-insensitively
    assert(airports_lookup("NOU", &la, &lo));
    assert(la < -21.0 && la > -23.0 && lo > 165.0 && lo < 167.0);
    double la2, lo2;
    assert(airports_lookup("nwww", &la2, &lo2));
    assert(la == la2 && lo == lo2);

    // unknown / malformed codes
    assert(!airports_lookup("XXX", &la, &lo));
    assert(!airports_lookup("ZZZZ", &la, &lo));
    assert(!airports_lookup("", &la, &lo));
    assert(!airports_lookup("NO", &la, &lo));
    assert(!airports_lookup("TOOLONG", &la, &lo));

    // distance sanity: Tontouta -> Sydney is ~1060 nm
    double sla, slo, nla, nlo;
    assert(airports_lookup("NOU", &nla, &nlo) && airports_lookup("SYD", &sla, &slo));
    double d = geo_dist_nm(nla, nlo, sla, slo);
    assert(d > 1000 && d < 1130);

    // westbound across the date line: Tontouta -> Papeete ~2540 nm (not 17000)
    double pla, plo;
    assert(airports_lookup("PPT", &pla, &plo));
    d = geo_dist_nm(nla, nlo, pla, plo);
    assert(d > 2400 && d < 2700);

    // elevation + IATA accessors (values from the Offto airport DB)
    int elev = -1;
    assert(airports_lookup_ex("NWWW", &la, &lo, &elev) && elev == 52);
    assert(airports_lookup_ex("NRT", NULL, NULL, &elev) && elev == 141);   // by IATA
    assert(airports_lookup_ex("YMML", NULL, NULL, &elev) && elev == 434);
    assert(!airports_lookup_ex("ZZZZ", NULL, NULL, &elev));
    assert(airports_iata("RJAA") && strcmp(airports_iata("RJAA"), "NRT") == 0);
    assert(airports_iata("nou") && strcmp(airports_iata("nou"), "NOU") == 0);
    assert(airports_iata("ZZZZ") == NULL);

    printf("test_airports: PASS\n");
    return 0;
}
