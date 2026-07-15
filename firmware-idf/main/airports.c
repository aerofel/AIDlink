// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "airports.h"
#include <string.h>
#include <ctype.h>

// Aircalin network + Pacific neighbours + common long-haul/diversion fields.
// Reference point per airport (ARP, ~2 decimal places is plenty for an NM
// to-go readout) + field elevation (ft MSL, from the Offto airport DB — used
// by the theoretical descent profile). Extend freely — lookup is linear over
// ~50 rows.
typedef struct { char iata[4]; char icao[5]; float lat, lon; int16_t elev_ft; } apt_t;

static const apt_t APT[] = {
    // New Caledonia
    { "NOU", "NWWW", -22.0146f, 166.2130f,  52 },   // Nouméa La Tontouta
    { "GEA", "NWWM", -22.2583f, 166.4728f,  10 },   // Nouméa Magenta
    { "ILP", "NWWE", -22.5889f, 167.4556f, 315 },   // Île des Pins
    { "LIF", "NWWL", -20.7748f, 167.2400f,  92 },   // Lifou
    { "MEE", "NWWR", -21.4817f, 168.0378f, 141 },   // Maré
    { "UVE", "NWWV", -20.6406f, 166.5731f,  23 },   // Ouvéa
    { "TGJ", "NWWA", -21.0961f, 167.8040f,   0 },   // Tiga (not in DB)
    { "KNQ", "NWWD", -21.0543f, 164.8372f,  23 },   // Koné
    { "TOU", "NWWU", -20.7900f, 165.2590f,  10 },   // Touho
    // Australia
    { "SYD", "YSSY", -33.9461f, 151.1772f,  21 },
    { "BNE", "YBBN", -27.3842f, 153.1175f,  13 },
    { "MEL", "YMML", -37.6733f, 144.8433f, 434 },
    { "CNS", "YBCS", -16.8858f, 145.7554f,  10 },
    { "OOL", "YBCG", -28.1644f, 153.5047f,  21 },
    { "PER", "YPPH", -31.9403f, 115.9670f,  67 },
    { "ADL", "YPAD", -34.9450f, 138.5306f,  20 },
    // New Zealand
    { "AKL", "NZAA", -37.0081f, 174.7917f,  23 },
    { "CHC", "NZCH", -43.4894f, 172.5322f, 123 },
    { "WLG", "NZWN", -41.3272f, 174.8053f,  41 },
    // South Pacific
    { "NAN", "NFFN", -17.7554f, 177.4434f,  59 },   // Nadi
    { "SUV", "NFNA", -18.0433f, 178.5592f,  17 },   // Suva
    { "PPT", "NTAA", -17.5537f, -149.6070f,  5 },   // Papeete
    { "RAR", "NCRG", -21.2027f, -159.8060f, 19 },   // Rarotonga
    { "VLI", "NVVV", -17.6993f, 168.3199f,  70 },   // Port Vila
    { "SON", "NVSS", -15.5050f, 167.2197f, 184 },   // Santo
    { "WLS", "NLWW", -13.2383f, -176.1990f, 79 },   // Wallis
    { "FUT", "NLWF", -14.3114f, -178.0659f, 20 },   // Futuna
    { "TBU", "NFTF", -21.2412f, -175.1496f, 126 },  // Tongatapu
    { "APW", "NSFA", -13.8300f, -171.9970f,  58 },  // Apia
    { "PPG", "NSTU", -14.3310f, -170.7105f,  32 },  // Pago Pago
    { "HIR", "AGGH",  -9.4280f, 160.0549f,   28 },  // Honiara
    { "POM", "AYPY",  -9.4434f, 147.2200f,  146 },  // Port Moresby
    { "GUM", "PGUM",  13.4834f, 144.7960f,  298 },  // Guam
    { "HNL", "PHNL",  21.3187f, -157.9225f,  13 },  // Honolulu
    // Asia
    { "NRT", "RJAA",  35.7647f, 140.3864f,  141 },  // Tokyo Narita
    { "HND", "RJTT",  35.5533f, 139.7811f,   35 },  // Tokyo Haneda
    { "KIX", "RJBB",  34.4342f, 135.2330f,   26 },  // Osaka Kansai
    { "NGO", "RJGG",  34.8584f, 136.8049f,   15 },  // Nagoya Chubu
    { "ICN", "RKSI",  37.4602f, 126.4407f,   23 },  // Seoul Incheon
    { "SIN", "WSSS",   1.3502f, 103.9944f,   22 },  // Singapore
    { "BKK", "VTBS",  13.6900f, 100.7501f,    5 },  // Bangkok
    { "HKG", "VHHH",  22.3080f, 113.9185f,   28 },  // Hong Kong
    { "PVG", "ZSPD",  31.1443f, 121.8083f,   13 },  // Shanghai Pudong
    { "DPS", "WADD",  -8.7482f, 115.1672f,   14 },  // Denpasar
    // Long-haul / other
    { "LAX", "KLAX",  33.9425f, -118.4081f, 125 },
    { "SFO", "KSFO",  37.6213f, -122.3790f,  13 },
    { "CDG", "LFPG",  49.0097f,   2.5479f,  392 },  // Paris CDG
};

static const apt_t *apt_find(const char *code) {
    if (!code || !code[0]) return NULL;
    char c[5] = {0};
    size_t n = strlen(code);
    if (n < 3 || n > 4) return NULL;
    for (size_t i = 0; i < n; i++) c[i] = (char)toupper((unsigned char)code[i]);
    for (unsigned i = 0; i < sizeof APT / sizeof APT[0]; i++) {
        const char *want = (n == 3) ? APT[i].iata : APT[i].icao;
        if (strcmp(c, want) == 0) return &APT[i];
    }
    return NULL;
}

bool airports_lookup(const char *code, double *lat, double *lon) {
    return airports_lookup_ex(code, lat, lon, NULL);
}

bool airports_lookup_ex(const char *code, double *lat, double *lon, int *elev_ft) {
    const apt_t *a = apt_find(code);
    if (!a) return false;
    if (lat) *lat = a->lat;
    if (lon) *lon = a->lon;
    if (elev_ft) *elev_ft = a->elev_ft;
    return true;
}

const char *airports_icao(const char *code) {
    const apt_t *a = apt_find(code);
    return a ? a->icao : NULL;
}

const char *airports_iata(const char *code) {
    const apt_t *a = apt_find(code);
    return a ? a->iata : NULL;
}
