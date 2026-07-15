// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Tiny embedded airport gazetteer for the onboard display's distance-to-arrival
// readout. Pure C (host-testable). Codes match either IATA (3) or ICAO (4).
#pragma once
#include <stdbool.h>

// Look up an airport by IATA or ICAO code (case-insensitive).
// Returns true and fills lat/lon (degrees) when found.
bool airports_lookup(const char *code, double *lat, double *lon);

// Canonical ICAO code for an IATA or ICAO code (case-insensitive).
// Returns NULL when the airport isn't in the table.
const char *airports_icao(const char *code);

// Lookup with field elevation (ft MSL; 0 when unsurveyed). Any out-param may
// be NULL. Same match rules as airports_lookup.
bool airports_lookup_ex(const char *code, double *lat, double *lon, int *elev_ft);

// IATA code for display ("NOU" reads better than "NWWW" on a 320 px line).
// NULL when the airport isn't in the table.
const char *airports_iata(const char *code);
