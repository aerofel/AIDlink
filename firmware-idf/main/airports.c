// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "airports.h"
#include <string.h>
#include <ctype.h>

// Aircalin network + Pacific neighbours + common long-haul/diversion fields.
// Reference point per airport (ARP, ~2 decimal places is plenty for an NM
// to-go readout). Extend freely — lookup is linear over ~50 rows.
typedef struct { char iata[4]; char icao[5]; float lat, lon; } apt_t;

static const apt_t APT[] = {
    // New Caledonia
    { "NOU", "NWWW", -22.0146f, 166.2130f },   // Nouméa La Tontouta
    { "GEA", "NWWM", -22.2583f, 166.4728f },   // Nouméa Magenta
    { "ILP", "NWWE", -22.5889f, 167.4556f },   // Île des Pins
    { "LIF", "NWWL", -20.7748f, 167.2400f },   // Lifou
    { "MEE", "NWWR", -21.4817f, 168.0378f },   // Maré
    { "UVE", "NWWV", -20.6406f, 166.5731f },   // Ouvéa
    { "TGJ", "NWWA", -21.0961f, 167.8040f },   // Tiga
    { "KNQ", "NWWD", -21.0543f, 164.8372f },   // Koné
    { "TOU", "NWWU", -20.7900f, 165.2590f },   // Touho
    // Australia
    { "SYD", "YSSY", -33.9461f, 151.1772f },
    { "BNE", "YBBN", -27.3842f, 153.1175f },
    { "MEL", "YMML", -37.6733f, 144.8433f },
    { "CNS", "YBCS", -16.8858f, 145.7554f },
    { "OOL", "YBCG", -28.1644f, 153.5047f },
    { "PER", "YPPH", -31.9403f, 115.9670f },
    { "ADL", "YPAD", -34.9450f, 138.5306f },
    // New Zealand
    { "AKL", "NZAA", -37.0081f, 174.7917f },
    { "CHC", "NZCH", -43.4894f, 172.5322f },
    { "WLG", "NZWN", -41.3272f, 174.8053f },
    // South Pacific
    { "NAN", "NFFN", -17.7554f, 177.4434f },   // Nadi
    { "SUV", "NFNA", -18.0433f, 178.5592f },   // Suva
    { "PPT", "NTAA", -17.5537f, -149.6070f },  // Papeete
    { "RAR", "NCRG", -21.2027f, -159.8060f },  // Rarotonga
    { "VLI", "NVVV", -17.6993f, 168.3199f },   // Port Vila
    { "SON", "NVSS", -15.5050f, 167.2197f },   // Santo
    { "WLS", "NLWW", -13.2383f, -176.1990f },  // Wallis
    { "FUT", "NLWF", -14.3114f, -178.0659f },  // Futuna
    { "TBU", "NFTF", -21.2412f, -175.1496f },  // Tongatapu
    { "APW", "NSFA", -13.8300f, -171.9970f },  // Apia
    { "PPG", "NSTU", -14.3310f, -170.7105f },  // Pago Pago
    { "HIR", "AGGH",  -9.4280f, 160.0549f },   // Honiara
    { "POM", "AYPY",  -9.4434f, 147.2200f },   // Port Moresby
    { "GUM", "PGUM",  13.4834f, 144.7960f },   // Guam
    { "HNL", "PHNL",  21.3187f, -157.9225f },  // Honolulu
    // Asia
    { "NRT", "RJAA",  35.7647f, 140.3864f },   // Tokyo Narita
    { "HND", "RJTT",  35.5533f, 139.7811f },   // Tokyo Haneda
    { "KIX", "RJBB",  34.4342f, 135.2330f },   // Osaka Kansai
    { "NGO", "RJGG",  34.8584f, 136.8049f },   // Nagoya Chubu
    { "ICN", "RKSI",  37.4602f, 126.4407f },   // Seoul Incheon
    { "SIN", "WSSS",   1.3502f, 103.9944f },   // Singapore
    { "BKK", "VTBS",  13.6900f, 100.7501f },   // Bangkok
    { "HKG", "VHHH",  22.3080f, 113.9185f },   // Hong Kong
    { "PVG", "ZSPD",  31.1443f, 121.8083f },   // Shanghai Pudong
    { "DPS", "WADD",  -8.7482f, 115.1672f },   // Denpasar
    // Long-haul / other
    { "LAX", "KLAX",  33.9425f, -118.4081f },
    { "SFO", "KSFO",  37.6213f, -122.3790f },
    { "CDG", "LFPG",  49.0097f,   2.5479f },   // Paris CDG
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
    const apt_t *a = apt_find(code);
    if (!a) return false;
    if (lat) *lat = a->lat;
    if (lon) *lon = a->lon;
    return true;
}

const char *airports_icao(const char *code) {
    const apt_t *a = apt_find(code);
    return a ? a->icao : NULL;
}
