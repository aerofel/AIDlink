// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Pure ADBP frame builders (see adbp_frame.h). Reproduces the v9 wire format:
// ARINC-834 <parameter> encoding, sync <response>, and the push <method> frame
// with the self-referential length="" fixed-point iteration + framing options.
#include "adbp_frame.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

#define XMLPROLOG "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"

char *adbp_xml_esc(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (const char *p = src ? src : ""; *p && o + 1 < cap; p++) {
        const char *r = NULL;
        switch (*p) { case '&': r="&amp;"; break; case '<': r="&lt;"; break;
                      case '>': r="&gt;"; break; case '"': r="&quot;"; break; default: break; }
        if (r) { size_t rl = strlen(r); if (o+rl+1 > cap) break; memcpy(dst+o, r, rl); o += rl; }
        else dst[o++] = *p;
    }
    dst[o] = 0;
    return dst;
}

static double norm180(double d) { while (d > 180) d -= 360; while (d < -180) d += 360; return d; }

void adbp_dead_reckon(pos_state_t *p, double dt_s) {
    if (!p->valid || p->fixed || !p->have_track) return;
    if (!(p->gs_kt > 1.0 && p->gs_kt <= 1500.0)) return;
    double dist_nm = p->gs_kt * (dt_s / 3600.0);
    double d = dist_nm / 3440.065;                    // angular distance (earth radius nm)
    double lat = p->lat * M_PI / 180.0, lon = p->lon * M_PI / 180.0, trk = p->track_deg * M_PI / 180.0;
    double lat2 = asin(sin(lat) * cos(d) + cos(lat) * sin(d) * cos(trk));
    double lon2 = lon + atan2(sin(trk) * sin(d) * cos(lat), cos(d) - sin(lat) * sin(lat2));
    p->lat = lat2 * 180.0 / M_PI;
    p->lon = lon2 * 180.0 / M_PI;
}

int adbp_parse_params(const char *req, char names[][ADBP_MAXNAME], int maxn) {
    int n = 0;
    const char *needle = "<parameter name=\"";
    const char *p = req;
    while (n < maxn && (p = strstr(p, needle))) {
        p += strlen(needle);
        int i = 0;
        while (*p && *p != '"' && i < ADBP_MAXNAME - 1) names[n][i++] = *p++;
        names[n][i] = 0;
        n++;
    }
    return n;
}

long adbp_tag_num(const char *req, const char *tag, long dflt) {
    char open[48]; snprintf(open, sizeof open, "<%s>", tag);
    const char *p = strstr(req, open);
    if (!p) return dflt;
    p += strlen(open);
    return strtol(p, NULL, 10);
}

// uppercase copy for case-insensitive name matching
static void upper(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) dst[i] = toupper((unsigned char)src[i]);
    dst[i] = 0;
}
static bool has(const char *hay, const char *needle) { return strstr(hay, needle) != NULL; }

// Emit one <parameter .../> for `name`. Returns bytes written; sets *matched.
static int param_xml(char *out, size_t cap, const char *name, const pos_state_t *p,
                     const aidlink_cfg_t *cfg, bool fresh, uint64_t stamp_ms, bool *matched) {
    char U[ADBP_MAXNAME]; upper(U, sizeof U, name);
    char nm[ADBP_MAXNAME]; adbp_xml_esc(nm, sizeof nm, name);
    *matched = true;
    char val[64]; int type = 0; bool ncd = !fresh;

    // helper macros to format
    #define VALF(fmt, x) do { snprintf(val, sizeof val, fmt, (x)); } while (0)

    if (has(U, "LAT") && !has(U, "GPSLATPF")) { type = 0; VALF("%.6f", p->lat); }
    else if (has(U, "LON") && !has(U, "GPSLONGPF")) { type = 0; VALF("%.6f", p->lon); }
    else if (has(U, "GPSLATPF") || has(U, "GPSLONGPF") || has(U, "FINE")) { type = 0; strcpy(val, "0.000000"); }
    else if (has(U, "TRK") || has(U, "TRACK") || has(U, "GPSTTRKA")) {
        type = 0; if (!p->have_track) ncd = true; VALF("%.2f", norm180(p->track_deg));
    }
    else if (has(U, "THDG") || has(U, "HDG") || has(U, "HEADING")) {
        type = 3; if (!p->have_track) ncd = true; VALF("%.2f", norm180(p->track_deg));
    }
    else if (has(U, "GS") || has(U, "GROUND")) {
        type = 0; double gs = (p->gs_kt >= 0 && p->gs_kt <= 1500) ? p->gs_kt : 0; VALF("%.1f", gs);
    }
    else if ((has(U, "ALT") && !has(U, "CORRECTION")) || has(U, "BARO_CORRECTION_ALTITUDE1")) {
        type = 0; double a = p->alt_ft > 1.0 ? p->alt_ft : 31000; VALF("%.0f", a);
    }
    else if (has(U, "GNSS_AVAIL") || (has(U, "AVAIL"))) { type = 6; VALF("%d", fresh ? 1 : 0); ncd = false; }
    else if (has(U, "FOM")) { type = 0; strcpy(val, "8.0"); ncd = false; }
    else if (has(U, "HDOP") || has(U, "VDOP") || has(U, "DILUTION")) { type = 0; strcpy(val, "0.8"); ncd = false; }
    else if (has(U, "INTEGRITYLIMIT") || has(U, "GPSHIL")) { type = 0; strcpy(val, "0.10"); ncd = false; }
    else if (has(U, "ACID") || has(U, "TAIL") || has(U, "REGIST") || has(U, "ACREG")) {
        type = 7; const char *s = p->tail[0] ? p->tail : cfg->ac_tail;
        char e[32]; adbp_xml_esc(e, sizeof e, s); snprintf(val, sizeof val, "%s", e); if (!s[0]) ncd = true;
    }
    else if (has(U, "FROMTO") || has(U, "CITYPAIR") || has(U, "CITY_PAIR")) {
        type = 7; if (!p->orig[0] || !p->dest[0]) ncd = true;
        char e[32]; snprintf(e, sizeof e, "%s%s", p->orig, p->dest); adbp_xml_esc(val, sizeof val, e);
    }
    else if (has(U, "FLT") || has(U, "FLIGHT")) {
        type = 7; adbp_xml_esc(val, sizeof val, p->flight); if (!p->flight[0]) ncd = true;
    }
    else if (has(U, "DEST") || has(U, "ADES") || has(U, "ARR")) {
        type = 7; adbp_xml_esc(val, sizeof val, p->dest); if (!p->dest[0]) ncd = true;
    }
    else if (has(U, "ORIG") || has(U, "ADEP") || has(U, "DEP")) {
        type = 7; adbp_xml_esc(val, sizeof val, p->orig); if (!p->orig[0]) ncd = true;
    }
    else if (has(U, "AIRCRAFTTYPE") || has(U, "ACTYP")) {
        type = 7; adbp_xml_esc(val, sizeof val, cfg->ac_type); if (!cfg->ac_type[0]) ncd = true;
    }
    else if (has(U, "WOW") || has(U, "WHEEL") || has(U, "WONW") || has(U, "PBRK") || has(U, "DOOR")) {
        type = 6; strcpy(val, "0"); ncd = false;
    }
    else { *matched = false; ncd = true; }   // unknown -> NCD

    if (ncd)
        return snprintf(out, cap, "<parameter name=\"%s\" validity=\"2\"/>", nm);
    return snprintf(out, cap, "<parameter name=\"%s\" validity=\"1\" type=\"%d\" value=\"%s\" time=\"%llu\"/>",
                    nm, type, val, (unsigned long long)stamp_ms);
    #undef VALF
}

int adbp_params_block(char *out, size_t cap, char names[][ADBP_MAXNAME], int n,
                      const pos_state_t *p, const aidlink_cfg_t *cfg,
                      bool fresh, uint64_t stamp_ms, bool *miss) {
    int o = 0;
    o += snprintf(out + o, cap - o, "<parameters>");
    bool any_miss = false;
    for (int i = 0; i < n && o < (int)cap - 128; i++) {
        bool matched;
        o += param_xml(out + o, cap - o, names[i], p, cfg, fresh, stamp_ms, &matched);
        if (!matched) any_miss = true;
    }
    o += snprintf(out + o, cap - o, "</parameters>");
    if (miss) *miss = any_miss;
    return o;
}

int adbp_wrap_resp(char *out, size_t cap, const char *method, int errorcode, const char *body) {
    return snprintf(out, cap, XMLPROLOG "\n<response method=\"%s\" errorcode=\"%d\">%s</response>",
                    method, errorcode, body);
}

int adbp_wrap_push(char *out, size_t cap, const char *method, const char *body,
                   bool with_prolog, const aidlink_cfg_t *cfg) {
    const char *prolog = with_prolog ? XMLPROLOG : "";
    if (cfg->frame_len == 2) {
        return snprintf(out, cap, "%s<method name=\"%s\">%s</method>", prolog, method, body);
    }
    // frame_len 0 (whole message incl prolog) or 1 (method element only):
    // solve the self-referential length by fixed-point iteration.
    int L = 0, prevL = -1;
    for (int guard = 0; guard < 8 && L != prevL; guard++) {
        prevL = L;
        int inner = snprintf(NULL, 0, "%s<method name=\"%s\" length=\"%d\">%s</method>",
                             prolog, method, L, body);
        int elem_only = snprintf(NULL, 0, "<method name=\"%s\" length=\"%d\">%s</method>",
                                 method, L, body);
        L = (cfg->frame_len == 0) ? inner : elem_only;
    }
    return snprintf(out, cap, "%s<method name=\"%s\" length=\"%d\">%s</method>", prolog, method, L, body);
}
