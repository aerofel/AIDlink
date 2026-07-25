// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "nmea.h"
#include <stdlib.h>
#include <string.h>

#define MAX_LINE   120   // longest sentence we accept; NMEA 0183 caps at 82
#define MAX_FIELDS 24

void nmea_reset(nmea_state_t *s) {
    memset(s, 0, sizeof *s);
    s->fix = NMEA_FIX_NONE;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool nmea_checksum_ok(const char *line) {
    if (!line || line[0] != '$') return false;
    const char *star = strchr(line, '*');
    if (!star || star == line + 1) return false;
    int hi = hexval(star[1]);
    if (hi < 0) return false;
    int lo = hexval(star[2]);
    if (lo < 0) return false;
    if (star[3] != 0) return false;                 // trailing junk after the sum

    unsigned sum = 0;
    for (const char *p = line + 1; p < star; p++) sum ^= (unsigned char)*p;
    return sum == (unsigned)((hi << 4) | lo);
}

// Split a copy of the sentence on commas. Returns the field count. Fields keep
// their order including empties, which matters: an empty field means "no data"
// and must never be treated as zero.
static int split(char *buf, char *f[], int cap) {
    int n = 0;
    f[n++] = buf;
    for (char *p = buf; *p && n < cap; p++) {
        if (*p == ',') { *p = 0; f[n++] = p + 1; }
    }
    return n;
}

static bool has(char *const f[], int n, int i) { return i < n && f[i][0]; }

// GGA's quality field is single-valued and authoritative for "is there a fix at
// all"; the per-constellation GSA dimensions only refine it to 2D or 3D. Without
// this gate a constellation that stops reporting leaves a stale 3D behind, which
// showed up on the bench as "3D fix, 0 satellites used, HDOP 99.99".
static void recompute_fix(nmea_state_t *s) {
    if (s->gga_quality == 0) { s->fix = NMEA_FIX_NONE; return; }
    nmea_fix_t best = NMEA_FIX_NONE;
    for (int i = 0; i < 6; i++) if (s->fix_sys[i] > best) best = s->fix_sys[i];
    // GGA says there is a fix, so never report less than 2D even if no GSA has
    // arrived yet this session.
    s->fix = (best == NMEA_FIX_NONE) ? NMEA_FIX_2D : best;
}

// "ddmm.mmmm" / "dddmm.mmmm" -> signed degrees. deg_digits is 2 for latitude,
// 3 for longitude. Returns false when the field is empty or malformed.
static bool parse_coord(const char *v, const char *hemi, int deg_digits, double *out) {
    if (!v || !v[0] || !hemi || !hemi[0]) return false;
    if ((int)strlen(v) < deg_digits + 3) return false;
    char dbuf[4] = {0};
    memcpy(dbuf, v, deg_digits);
    double deg = atof(dbuf);
    double min = atof(v + deg_digits);
    double d = deg + min / 60.0;
    if (hemi[0] == 'S' || hemi[0] == 'W') d = -d;
    *out = d;
    return true;
}

// Days since the Unix epoch for a civil date. Avoids timegm(), which is not
// portable across the hosts this test suite runs on.
static long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe - 719468L;
}

// RMC time "hhmmss.ss" + date "ddmmyy" -> epoch ms. 0 when either is absent.
static uint64_t parse_utc(const char *tod, const char *date) {
    if (!tod || strlen(tod) < 6 || !date || strlen(date) < 6) return 0;
    int hh = (tod[0] - '0') * 10 + (tod[1] - '0');
    int mi = (tod[2] - '0') * 10 + (tod[3] - '0');
    int se = (tod[4] - '0') * 10 + (tod[5] - '0');
    int dd = (date[0] - '0') * 10 + (date[1] - '0');
    int mo = (date[2] - '0') * 10 + (date[3] - '0');
    int yy = (date[4] - '0') * 10 + (date[5] - '0');
    if (mo < 1 || mo > 12 || dd < 1 || dd > 31) return 0;
    long days = days_from_civil(2000 + yy, mo, dd);
    return ((uint64_t)days * 86400ULL + hh * 3600ULL + mi * 60ULL + se) * 1000ULL;
}

// Per-constellation in-view counts are stored separately and summed, so a
// re-reported talker replaces its own count instead of accumulating.
static void gsv_store(nmea_state_t *s, const char *talker, int count) {
    if      (!strncmp(talker, "GP", 2)) s->sats_gps  = count;
    else if (!strncmp(talker, "GL", 2)) s->sats_glo  = count;
    else if (!strncmp(talker, "GA", 2)) s->sats_gal  = count;
    else if (!strncmp(talker, "GB", 2)) s->sats_bds  = count;
    else if (!strncmp(talker, "GQ", 2)) s->sats_qzss = count;
    else return;
    s->sats_view = s->sats_gps + s->sats_glo + s->sats_gal + s->sats_bds + s->sats_qzss;
}

bool nmea_line(nmea_state_t *s, const char *line) {
    if (!s || !line || line[0] != '$') return false;
    size_t len = strlen(line);
    if (len < 6 || len >= MAX_LINE) return false;

    char buf[MAX_LINE];
    memcpy(buf, line, len + 1);
    char *star = strchr(buf, '*');
    if (star) *star = 0;                       // drop the checksum before splitting

    char *f[MAX_FIELDS];
    int n = split(buf, f, MAX_FIELDS);
    if (n < 2) return false;

    const char *talker = f[0] + 1;             // skip '$'
    if (strlen(talker) < 5) return false;
    const char *type = talker + 2;             // "GGA", "RMC", "GSA", "GSV"

    if (!strcmp(type, "GGA")) {
        // $xxGGA,time,lat,NS,lon,EW,quality,numSV,HDOP,alt,M,sep,M,age,station
        double lat, lon;
        if (has(f, n, 2) && has(f, n, 3) && has(f, n, 4) && has(f, n, 5) &&
            parse_coord(f[2], f[3], 2, &lat) && parse_coord(f[4], f[5], 3, &lon)) {
            s->lat = lat; s->lon = lon; s->have_pos = true;
        } else {
            s->have_pos = false;               // keep the last position; flag it stale
        }
        if (has(f, n, 6)) s->gga_quality = atoi(f[6]);
        if (has(f, n, 7)) s->sats_used = atoi(f[7]);
        if (has(f, n, 8)) s->hdop = atof(f[8]);
        if (has(f, n, 9)) s->alt_m = atof(f[9]);
        recompute_fix(s);
        return true;
    }

    if (!strcmp(type, "RMC")) {
        // $xxRMC,time,status,lat,NS,lon,EW,spd,cog,date,mv,mvEW,mode,navStatus
        s->rmc_valid = has(f, n, 2) && f[2][0] == 'A';
        if (has(f, n, 7)) s->gs_kt = atof(f[7]);
        if (has(f, n, 8)) s->track_deg = atof(f[8]);
        if (has(f, n, 1) && has(f, n, 9)) {
            uint64_t t = parse_utc(f[1], f[9]);
            if (t) s->utc_ms = t;
        }
        return true;
    }

    if (!strcmp(type, "GSA")) {
        // $xxGSA,mode,fixType,sv1..sv12,PDOP,HDOP,VDOP,systemId
        if (n >= 17 && f[16][0]) s->hdop = atof(f[16]);

        nmea_fix_t fx = NMEA_FIX_NONE;
        if (has(f, n, 2)) {
            int v = atoi(f[2]);
            fx = (v == 3) ? NMEA_FIX_3D : (v == 2) ? NMEA_FIX_2D : NMEA_FIX_NONE;
        }
        // Populated PRN slots (fields 3..14) = satellites this constellation is
        // contributing to the solution.
        int used = 0;
        for (int i = 3; i <= 14 && i < n; i++) if (f[i][0]) used++;

        // f[15]=PDOP f[16]=HDOP f[17]=VDOP f[18]=systemId. Reading 17 here gives
        // VDOP, which parses as a plausible-looking constellation id.
        int sys = (n >= 19 && f[18][0]) ? atoi(f[18]) : 0;
        switch (sys) {
            case 1: s->used_gps  = used; break;
            case 2: s->used_glo  = used; break;
            case 3: s->used_gal  = used; break;
            case 4: s->used_bds  = used; break;
            case 5: s->used_qzss = used; break;
            default: break;      // no systemId (NMEA < 4.10): cannot attribute
        }

        // Record this constellation's dimension, then expose the best of them.
        // Replacing per system means an empty GSA clears its own contribution
        // without erasing what another constellation reported this cycle.
        if (sys >= 1 && sys <= 5) s->fix_sys[sys] = fx;
        else                      s->fix_sys[0]   = fx;   // unattributed talker
        recompute_fix(s);
        return true;
    }

    if (!strcmp(type, "GSV")) {
        // $xxGSV,numMsg,msgNum,numSV,...
        if (has(f, n, 3)) gsv_store(s, talker, atoi(f[3]));
        return true;
    }

    return false;
}
