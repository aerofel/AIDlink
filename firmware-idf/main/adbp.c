// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// ADBP server socket/task layer. One task owns a listening TCP socket on
// cfg.adbp_port (command channel) and a table of subscriptions, each with its
// own outbound push socket to the client's publishport. Wire format lives in
// adbp_frame.c; ownship state comes from pos.h.
#include "adbp.h"
#include "adbp_frame.h"
#include "pos.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "lwip/sockets.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_SUBS 4   // push socket per EFB; the socket pool (16) can't feed more
static const char *TAG = "adbp";

typedef struct {
    bool     active;
    uint32_t ip;            // client IP, network order
    uint16_t port;          // advertised publishport
    bool     on_event;
    uint32_t period_ms, last_push_ms, last_fix_seen;
    char     params[1400];
    int      sock;          // push socket (-1 = not connected)
    bool     was_connected;
    uint32_t push_count;
} sub_t;

static sub_t s_subs[MAX_SUBS];
static const aidlink_cfg_t *CFG;
static volatile uint32_t s_push_seq;   // ++ on every frame pushed to an EFB

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static uint64_t stamp_ms(uint64_t utc_ms) {
    time_t t = time(NULL);
    if (t > 1700000000) return (uint64_t)t * 1000;   // SNTP-synced wall clock
    if (utc_ms > 1000000000000ULL) return utc_ms;    // plausible source time
    return 1782000000000ULL + (now_ms() % 1000);     // synthetic floor
}

// Snapshot pos + dead-reckon, then build <parameters>. Returns *fresh + *miss.
static int build_params(char *out, size_t cap, char names[][ADBP_MAXNAME], int n, bool *miss) {
    pos_state_t p; pos_get(&p);
    uint32_t age = now_ms() - p.last_fix_ms;
    bool fresh = p.valid && age < CFG->stale_ms;
    if (fresh) {
        double dt = age / 1000.0; if (dt > CFG->stale_ms / 1000.0) dt = CFG->stale_ms / 1000.0;
        adbp_dead_reckon(&p, dt);
    }
    return adbp_params_block(out, cap, names, n, &p, CFG, fresh, stamp_ms(p.utc_ms), miss);
}

// Read an ADBP request from a connected socket (until </method> or timeout).
static int read_request(int s, char *buf, int cap) {
    int off = 0; uint32_t t0 = now_ms();
    struct timeval tv = { .tv_sec = 0, .tv_usec = 60000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (off < cap - 1) {
        int r = recv(s, buf + off, cap - 1 - off, 0);
        if (r <= 0) { if (off > 0) break; if (now_ms() - t0 > 60) break; continue; }
        off += r; buf[off] = 0;
        if (strstr(buf, "</method>")) break;
        if (now_ms() - t0 > 250) break;
    }
    buf[off] = 0;
    return off;
}

static void drop_subs_from_ip(uint32_t ip) {
    for (int i = 0; i < MAX_SUBS; i++) if (s_subs[i].active && s_subs[i].ip == ip) {
        if (s_subs[i].sock >= 0) close(s_subs[i].sock);
        memset(&s_subs[i], 0, sizeof s_subs[i]); s_subs[i].sock = -1;
    }
}

static void handle_request(int cs, uint32_t peer_ip) {
    static char req[2048], body[3072], resp[4096];
    int n = read_request(cs, req, sizeof req);
    if (n <= 0) { close(cs); return; }

    char names[ADBP_MAXPARAMS][ADBP_MAXNAME];
    int np = adbp_parse_params(req, names, ADBP_MAXPARAMS);
    bool miss = false;

    // method name verbatim from <method name="..."> — handles FOMAX's
    // getParameters / unsubscribe as well as the Viasat-style names.
    char method[48] = "?";
    { const char *m = strstr(req, "name=\"");
      if (m) { m += 6; int i = 0; while (m[i] && m[i] != '"' && i < (int)sizeof method - 1) { method[i] = m[i]; i++; } method[i] = 0; } }
    char rmethod[64]; snprintf(rmethod, sizeof rmethod, "%sResponse", method);
    bool is_get   = strstr(req, "getParameters") || strstr(req, "getAvionicParameters");
    bool is_sub   = strstr(req, "subscribeAvionicParameters") != NULL;
    bool is_unsub = strstr(req, "unSubscribe") || strstr(req, "unsubscribe");
    logln("ADBP %s (%d params) from %u.%u.%u.%u", method, np,
          (unsigned)(peer_ip & 0xFF), (unsigned)((peer_ip >> 8) & 0xFF),
          (unsigned)((peer_ip >> 16) & 0xFF), (unsigned)((peer_ip >> 24) & 0xFF));

    bool data_sent = false;   // drives the "data sent" LED blue-flash
    if (is_get && !is_sub) {
        build_params(body, sizeof body, names, np, &miss);
        adbp_wrap_resp(resp, sizeof resp, rmethod, miss ? 8 : 0, body);
        data_sent = true;
    } else if (is_sub) {
        bool on_event = strstr(req, "OnEvent") != NULL;
        long pport = adbp_tag_num(req, "publishport", 0);
        long per = adbp_tag_num(req, "refreshperiod", 5000); if (per < 100) per = 5000;
        drop_subs_from_ip(peer_ip);
        if (pport > 0) {
            int idx = -1; for (int i = 0; i < MAX_SUBS; i++) if (!s_subs[i].active) { idx = i; break; }
            if (idx >= 0) {
                sub_t *su = &s_subs[idx]; memset(su, 0, sizeof *su);
                su->active = true; su->ip = peer_ip; su->port = (uint16_t)pport;
                su->on_event = on_event; su->period_ms = per; su->sock = -1;
                // store the space-separated param name list
                int o = 0;
                for (int i = 0; i < np && o < (int)sizeof(su->params) - ADBP_MAXNAME - 2; i++)
                    o += snprintf(su->params + o, sizeof(su->params) - o, "%s%s", i ? " " : "", names[i]);
                ESP_LOGI(TAG, "subscribe from %u.%u.%u.%u:%ld per=%ldms onEvent=%d np=%d",
                         (unsigned)(peer_ip & 0xFF), (unsigned)((peer_ip >> 8) & 0xFF),
                         (unsigned)((peer_ip >> 16) & 0xFF), (unsigned)((peer_ip >> 24) & 0xFF),
                         pport, per, on_event, np);
            } else ESP_LOGW(TAG, "no free subscription slot");
        }
        build_params(body, sizeof body, names, np, &miss);
        adbp_wrap_resp(resp, sizeof resp, rmethod, 0, body);
        data_sent = true;
    } else if (is_unsub) {
        long pport = adbp_tag_num(req, "publishport", 0);
        for (int i = 0; i < MAX_SUBS; i++)
            if (s_subs[i].active && s_subs[i].ip == peer_ip && (pport == 0 || s_subs[i].port == pport)) {
                if (s_subs[i].sock >= 0) close(s_subs[i].sock);
                memset(&s_subs[i], 0, sizeof s_subs[i]); s_subs[i].sock = -1;
            }
        adbp_wrap_resp(resp, sizeof resp, rmethod, 0, "<parameters></parameters>");
    } else {
        adbp_wrap_resp(resp, sizeof resp, "UnknownMethod", 2, "<parameters></parameters>");
    }

    send(cs, resp, strlen(resp), 0);
    if (data_sent) s_push_seq++;   // LED: blue-flash on any position data sent to the EFB (poll or subscribe)
    vTaskDelay(pdMS_TO_TICKS(5));
    close(cs);
}

static int connect_push(sub_t *su) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        // The smoking gun for the in-flight position dropout: the lwIP socket
        // pool is exhausted, so we cannot open a push socket to the EFB and the
        // aircraft position silently disappears from Jeppesen. Logged (not
        // inferred) so /log shows it directly if the pool is ever tight again.
        logln("[adbp] socket() FAILED — pool exhausted, cannot push to EFB");
        return -1;
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_addr.s_addr = su->ip;
    a.sin_port = htons(su->port ? su->port : CFG->ds_port);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) { close(s); return -1; }
    int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return s;
}

static void push_subs(void) {
    pos_state_t p; pos_get(&p);
    uint32_t cur_fix = p.last_fix_ms, now = now_ms();
    static char body[3072], msg[4096];

    for (int i = 0; i < MAX_SUBS; i++) {
        sub_t *su = &s_subs[i];
        if (!su->active) continue;
        bool due = su->on_event ? (cur_fix != su->last_fix_seen) : (now - su->last_push_ms >= su->period_ms);
        if (!due) continue;

        if (su->sock < 0) {
            su->sock = connect_push(su);
            if (su->sock < 0) { su->last_push_ms = now; continue; }
            su->was_connected = true; su->push_count = 0;
        }

        // rebuild the param name array from the stored list
        char names[ADBP_MAXPARAMS][ADBP_MAXNAME]; int np = 0;
        char tmp[1400]; strlcpy(tmp, su->params, sizeof tmp);
        for (char *tok = strtok(tmp, " "); tok && np < ADBP_MAXPARAMS; tok = strtok(NULL, " "))
            strlcpy(names[np++], tok, ADBP_MAXNAME);
        bool miss;
        build_params(body, sizeof body, names, np, &miss);

        const char *method = su->on_event ? "onEventAvionicParameters" : "publishAvionicParameters";
        bool with_prolog = CFG->frame_prolog_each || su->push_count == 0;
        int mlen = adbp_wrap_push(msg, sizeof msg, method, body, with_prolog, CFG);
        // delimiter
        char delim[3] = {0}; int dl = 0;
        switch (CFG->frame_delim) { case 1: delim[0]='\r'; delim[1]='\n'; dl=2; break;
                                    case 2: delim[0]='\n'; dl=1; break; case 3: delim[0]=0; dl=1; break; }

        int w = send(su->sock, msg, mlen, 0);
        if (w <= 0) { close(su->sock); su->sock = -1; su->was_connected = false; }
        else { if (dl) send(su->sock, delim, dl, 0); su->push_count++; s_push_seq++; }

        su->last_push_ms = now; su->last_fix_seen = cur_fix;
    }
}

static void adbp_task(void *arg) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(CFG->adbp_port);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0) { ESP_LOGE(TAG, "bind :%u failed", CFG->adbp_port); vTaskDelete(NULL); return; }
    listen(ls, 4);
    ESP_LOGI(TAG, "[ADBP] server on :%u", CFG->adbp_port);

    for (;;) {
        fd_set r; FD_ZERO(&r); FD_SET(ls, &r);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };   // 200 ms -> ~5 Hz push cadence
        if (select(ls + 1, &r, NULL, NULL, &tv) > 0 && FD_ISSET(ls, &r)) {
            struct sockaddr_in ca; socklen_t cl = sizeof ca;
            int cs = accept(ls, (struct sockaddr *)&ca, &cl);
            if (cs >= 0) handle_request(cs, ca.sin_addr.s_addr);
        }
        push_subs();
    }
}

bool adbp_feeding(void) {
    uint32_t now = now_ms();
    for (int i = 0; i < MAX_SUBS; i++)
        if (s_subs[i].active && s_subs[i].push_count > 0 && now - s_subs[i].last_push_ms < 5000)
            return true;
    return false;
}

uint32_t adbp_push_seq(void) { return s_push_seq; }

void adbp_start(const aidlink_cfg_t *cfg) {
    CFG = cfg;
    for (int i = 0; i < MAX_SUBS; i++) s_subs[i].sock = -1;
    xTaskCreate(adbp_task, "adbp", 8192, NULL, 5, NULL);
}
