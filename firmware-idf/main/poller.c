// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Position poller + emulator. apply_fix() and sim_step() write the shared pos
// state; poll_once() fetches and parses the configured source. Track and GS are
// derived from successive fixes when the source doesn't provide them.
#include "poller.h"
#include "pos.h"
#include "geo.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

static const char *TAG = "poll";
static aidlink_cfg_t *CFG;

// producer-side previous fix (for track/GS derivation)
static bool s_have_prev;
static double s_prev_lat, s_prev_lon;
static uint32_t s_prev_ms;

// live poll status (surfaced on the web /status page later)
static bool s_poll_ok;
static uint32_t s_poll_at_ms;
static char s_poll_msg[64] = "no poll yet";

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// Apply a live fix. gs<0 or track unknown -> derive from the previous fix.
static void apply_fix(double lat, double lon, double alt, double gs, double track,
                      bool have_track, uint64_t utc_ms, bool sim,
                      const char *flight, const char *tail, const char *orig, const char *dest) {
    pos_state_t p; pos_get(&p);
    uint32_t t = now_ms();

    double dgs = gs, dtrk = track; bool dhave = have_track;
    if (s_have_prev) {
        double dt_s = (t - s_prev_ms) / 1000.0;
        double dnm = geo_dist_nm(s_prev_lat, s_prev_lon, lat, lon);
        if (dt_s > 0.5 && dnm > 0.02) {
            double derived_gs = dnm / (dt_s / 3600.0);
            if (gs < 0) dgs = derived_gs;
            if (!have_track) { dtrk = geo_bearing_deg(s_prev_lat, s_prev_lon, lat, lon); dhave = true; }
        }
    }
    if (dgs < 0) dgs = 0;
    if (dgs > 1500) dgs = 1500;

    p.valid = true; p.simulated = sim; p.fixed = false;
    p.lat = lat; p.lon = lon; p.alt_ft = alt;
    p.gs_kt = dgs; p.track_deg = dtrk; p.have_track = dhave;
    if (utc_ms) p.utc_ms = utc_ms;
    p.last_fix_ms = t;
    p.service_avail = true;
    if (flight) strlcpy(p.flight, flight, sizeof p.flight);
    if (tail) strlcpy(p.tail, tail, sizeof p.tail);
    if (orig) strlcpy(p.orig, orig, sizeof p.orig);
    if (dest) strlcpy(p.dest, dest, sizeof p.dest);
    pos_set(&p);

    // A live tail number is the truth: replace the configured aircraft identity
    // (placeholder F-XXXX or a previous airframe) and persist it. Once per change.
    if (!sim && tail && tail[0] && strcmp(tail, CFG->ac_tail) != 0) {
        ESP_LOGI(TAG, "aircraft identity: %s -> %s (from live data)", CFG->ac_tail, tail);
        strlcpy(CFG->ac_tail, tail, sizeof CFG->ac_tail);
        cfg_save(CFG);
    }

    s_prev_lat = lat; s_prev_lon = lon; s_prev_ms = t; s_have_prev = true;
}

// Emulator: emit a FIXED position at cfg.sim* (matches v9 — does not advance;
// fixed=true so the ADBP consumer-side dead-reckoner leaves it in place).
static void sim_step(uint32_t dt_ms) {
    (void)dt_ms;
    pos_state_t p; pos_get(&p);
    double gs = CFG->sim_gs; if (gs < 0) gs = 0; if (gs > 1500) gs = 1500;
    p.valid = true; p.simulated = true; p.fixed = true; p.have_track = true;
    p.lat = CFG->sim_lat; p.lon = CFG->sim_lon; p.alt_ft = CFG->sim_alt;
    p.gs_kt = gs; p.track_deg = CFG->sim_trk;
    p.last_fix_ms = now_ms();
    time_t tt = time(NULL); if (tt > 1700000000) p.utc_ms = (uint64_t)tt * 1000;
    strlcpy(p.flight, CFG->ac_tail, sizeof p.flight);
    strlcpy(p.tail, CFG->ac_tail, sizeof p.tail);
    pos_set(&p);
}

// ---- HTTP fetch into a heap buffer ----
typedef struct { char *buf; int len, cap; long date; } fetch_t;

// RFC 7231 date ("Sun, 06 Jul 2026 13:59:00 GMT") -> epoch seconds, 0 if bad.
// Every HTTP response carries one, so a successful poll disciplines the clock
// even from sources with no time field (Panasonic) and with SNTP unreachable.
static long parse_http_date(const char *s) {
    static const char MON[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int d, y, h, mi, se;
    const char *p = strchr(s, ',');
    if (!p || sscanf(p + 1, " %d %3s %d %d:%d:%d", &d, mon, &y, &h, &mi, &se) != 6) return 0;
    const char *f = strstr(MON, mon);
    if (!f || y < 2025 || y > 2100) return 0;
    int m = (int)(f - MON) / 3 + 1;
    // days-from-civil (Howard Hinnant), same as poller_sources.c
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097 + (long)doe - 719468;
    return days * 86400L + h * 3600L + mi * 60L + se;
}

static esp_err_t http_evt(esp_http_client_event_t *e) {
    fetch_t *f = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        if (f->len + e->data_len < f->cap) { memcpy(f->buf + f->len, e->data, e->data_len); f->len += e->data_len; f->buf[f->len] = 0; }
    } else if (e->event_id == HTTP_EVENT_ON_HEADER) {
        if (e->header_key && e->header_value && strcasecmp(e->header_key, "Date") == 0)
            f->date = parse_http_date(e->header_value);
    }
    return ESP_OK;
}

// Fetch url into out (cap). Returns body length or -1; *date_out gets the
// response's Date header as epoch seconds (0 if absent/unparseable).
static int http_get(const char *url, char *out, int cap, long *date_out) {
    fetch_t f = { .buf = out, .len = 0, .cap = cap };
    esp_http_client_config_t c = {
        .url = url, .event_handler = http_evt, .user_data = &f,
        .timeout_ms = 5000,
        .skip_cert_common_name_check = true,   // mirror v9 setInsecure() for the HTTPS Viasat endpoint
        .crt_bundle_attach = NULL,
        .disable_auto_redirect = true,         // v9 does not follow redirects
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    esp_http_client_set_header(h, "User-Agent", "aidlink");
    esp_err_t e = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (date_out) *date_out = f.date;
    if (e != ESP_OK || status != 200) return -1;
    return f.len;
}

// forward decl — source parsers live in poller_sources.c
bool poller_parse_viasat(const char *json, double *lat, double *lon, double *alt,
                         double *gs, double *track, bool *have_track, uint64_t *utc_ms,
                         char *flight, char *tail, char *orig, char *dest);
bool poller_parse_panasonic(const char *json, double *lat, double *lon, double *alt,
                            double *gs, double *track, bool *have_track, uint64_t *utc_ms,
                            char *flight, char *tail, char *orig, char *dest);

static void poll_once(void) {
    const char *url;
    switch (CFG->src_type) {
        case 0: url = "https://wifi.inflight.viasat.com/ac/flight/info"; break;
        case 1: url = "http://services.inflightpanasonic.aero/inflight/services/flightdata/v1/flightdata"; break;
        default: url = CFG->vs_url; break;
    }
    static char body[4096];
    long http_date = 0;
    int n = http_get(url, body, sizeof body, &http_date);
    if (n <= 0) { s_poll_ok = false; strlcpy(s_poll_msg, "fetch failed", sizeof s_poll_msg); return; }

    // Discipline the system clock from the response's Date header (1 s grain);
    // SNTP, when it reaches a server, wins by keeping the delta under the gate.
    if (http_date > 1750000000L) {
        time_t now = time(NULL);
        long long delta = (long long)now - http_date;
        if (delta < -3 || delta > 3) {
            struct timeval tv = { .tv_sec = (time_t)http_date };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "clock set from HTTP Date (was off %lld s)", delta);
        }
    }

    double lat, lon, alt = 0, gs = -1, track = 0; bool have_track = false; uint64_t utc = 0;
    char flight[16] = "", tail[12] = "", orig[8] = "", dest[8] = "";
    bool ok = (CFG->src_type == 1)
        ? poller_parse_panasonic(body, &lat, &lon, &alt, &gs, &track, &have_track, &utc, flight, tail, orig, dest)
        : poller_parse_viasat(body, &lat, &lon, &alt, &gs, &track, &have_track, &utc, flight, tail, orig, dest);
    if (!ok) { s_poll_ok = false; strlcpy(s_poll_msg, "parse failed", sizeof s_poll_msg); return; }

    apply_fix(lat, lon, alt, gs, track, have_track, utc, false, flight, tail, orig, dest);
    s_poll_ok = true; s_poll_at_ms = now_ms(); strlcpy(s_poll_msg, "ok", sizeof s_poll_msg);
}

static void poller_task(void *arg) {
    uint32_t last_poll = 0, last_sim = now_ms();
    for (;;) {
        uint32_t t = now_ms();
        if (CFG->sim_enable) {
            uint32_t dt = t - last_sim; last_sim = t;
            sim_step(dt);
        } else {
            if (t - last_poll >= CFG->poll_ms) { last_poll = t; poll_once(); }
            // stale watchdog
            pos_state_t p; pos_get(&p);
            if (p.valid && !p.simulated && now_ms() - p.last_fix_ms > CFG->stale_ms) pos_mark_stale();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void poller_status(bool *ok, uint32_t *at_ms, char *msg, unsigned msgcap) {
    if (ok) *ok = s_poll_ok;
    if (at_ms) *at_ms = s_poll_at_ms;
    if (msg) strlcpy(msg, s_poll_msg, msgcap);
}

void poller_start(aidlink_cfg_t *cfg) {
    CFG = cfg;
    xTaskCreate(poller_task, "poller", 8192, NULL, 4, NULL);
}
