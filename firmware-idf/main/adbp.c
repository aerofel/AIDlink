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
#include "supervisor.h"
#include "netcore.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include "lwip/sockets.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_SUBS 4   // push socket per EFB (two crew iPads, each on Wi-Fi and cable)
// A departed EFB (asleep, out of range, unplugged) never unsubscribes. Each
// push attempt to it is a TCP connect that lwIP would otherwise retry for tens
// of seconds while this single task serves every EFB, so connects are capped,
// and after a few failures the dead subscriber is only retried every few
// seconds. It keeps its slot — Jeppesen resumes by itself when the iPad comes
// back — unless a new EFB needs the slot (see take_slot()).
#define PUSH_CONNECT_MS   1000     // an iPad in Wi-Fi power save can take ~300 ms to answer
#define PUSH_FAILS_SLOW   3        // consecutive failed connects before backing off
#define PUSH_RETRY_SLOW_MS 5000
static const char *TAG = "adbp";

typedef struct {
    bool     active;
    uint32_t ip;            // client IP, network order
    uint16_t port;          // advertised publishport
    bool     on_event;
    uint32_t period_ms, last_push_ms, last_fix_seen;
    adbp_pstate_t last_state;   // freshness state at the last push (edge detection)
    char     params[1400];
    int      sock;          // push socket (-1 = not connected)
    bool     was_connected;
    uint32_t push_count;
    uint32_t fails;         // consecutive failed connects (0 = reachable)
    uint32_t last_ok_ms;    // subscribe time, then last successful push
} sub_t;

// Subscriptions and the XML work buffers (~22 KB) live in PSRAM where there is
// some, allocated in adbp_start(): internal SRAM is the scarce resource here.
static sub_t *s_subs;
typedef struct {
    char req[2048], body[3072], resp[4096];   // handle_request()
    char pbody[3072], msg[4096];              // push_subs()
} adbp_bufs_t;
static adbp_bufs_t *B;
static const aidlink_cfg_t *CFG;
static volatile uint32_t s_push_seq;   // ++ on every frame pushed to an EFB

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static uint64_t stamp_ms(uint64_t utc_ms) {
    time_t t = time(NULL);
    if (t > 1700000000) return (uint64_t)t * 1000;   // SNTP-synced wall clock
    if (utc_ms > 1000000000000ULL) return utc_ms;    // plausible source time
    return 1782000000000ULL + (now_ms() % 1000);     // synthetic floor
}

// Current freshness state of the shared position, per spec 2026-08-13.
static adbp_pstate_t pos_state_now(void) {
    pos_state_t p; pos_get(&p);
    return adbp_classify(p.valid, p.last_fix_ms, now_ms(), CFG->stale_ms, CFG->dr_hold_ms);
}

const char *adbp_pos_state_str(void) {
    if (!CFG) return "ncd";
    switch (pos_state_now()) {
        case ADBP_POS_FRESH: return "fresh";
        case ADBP_POS_DR:    return "dr";
        default:             return "ncd";
    }
}

// Snapshot pos, classify, dead-reckon (fresh AND during the DR hold), then
// build <parameters>.
static int build_params(char *out, size_t cap, char names[][ADBP_MAXNAME], int n, bool *miss) {
    pos_state_t p; pos_get(&p);
    uint32_t age = now_ms() - p.last_fix_ms;
    adbp_pstate_t st = adbp_classify(p.valid, p.last_fix_ms, now_ms(),
                                     CFG->stale_ms, CFG->dr_hold_ms);
    if (st != ADBP_POS_NCD) {
        // In the DR hold the arbiter has already cleared p.valid; the retained
        // lat/lon/track/gs carry the extrapolation, so revalidate the local copy.
        if (st == ADBP_POS_DR) p.valid = true;
        double dt = age / 1000.0;
        double cap_s = (CFG->stale_ms + CFG->dr_hold_ms) / 1000.0;
        if (dt > cap_s) dt = cap_s;
        adbp_dead_reckon(&p, dt);
    }
    return adbp_params_block(out, cap, names, n, &p, CFG, st, age / 1000,
                             stamp_ms(p.utc_ms), miss);
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

static void ip_str(uint32_t ip, char *out, size_t n) {
    snprintf(out, n, "%u.%u.%u.%u", (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
             (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF));
}

// A free slot, or else the slot of the subscriber that has been unreachable the
// longest. Without eviction, four EFBs that left without unsubscribing (an iPad
// on Wi-Fi and on the cable counts twice) locked every new EFB out until reboot.
static int take_slot(void) {
    int victim = -1;
    for (int i = 0; i < MAX_SUBS; i++) {
        if (!s_subs[i].active) return i;
        if (s_subs[i].fails &&
            (victim < 0 || (int32_t)(s_subs[i].last_ok_ms - s_subs[victim].last_ok_ms) < 0))
            victim = i;
    }
    if (victim >= 0) {
        char ip[16]; ip_str(s_subs[victim].ip, ip, sizeof ip);
        logln("[adbp] evicting unreachable subscriber %s (silent %lus) for a new EFB", ip,
              (unsigned long)((now_ms() - s_subs[victim].last_ok_ms) / 1000));
        if (s_subs[victim].sock >= 0) close(s_subs[victim].sock);
    }
    return victim;
}

static void handle_request(int cs, uint32_t peer_ip) {
    char *req = B->req, *body = B->body, *resp = B->resp;
    int n = read_request(cs, req, sizeof B->req);
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
    char pip[16]; ip_str(peer_ip, pip, sizeof pip);
    logln_ram("ADBP %s (%d params) from %s", method, np, pip);

    bool data_sent = false;   // drives the "data sent" LED blue-flash
    if (is_get && !is_sub) {
        build_params(body, sizeof B->body, names, np, &miss);
        adbp_wrap_resp(resp, sizeof B->resp, rmethod, miss ? 8 : 0, body);
        data_sent = true;
    } else if (is_sub) {
        bool on_event = strstr(req, "OnEvent") != NULL;
        long pport = adbp_tag_num(req, "publishport", 0);
        long per = adbp_tag_num(req, "refreshperiod", 5000); if (per < 100) per = 5000;
        drop_subs_from_ip(peer_ip);
        if (pport > 0) {
            int idx = take_slot();
            if (idx >= 0) {
                sub_t *su = &s_subs[idx]; memset(su, 0, sizeof *su);
                su->active = true; su->ip = peer_ip; su->port = (uint16_t)pport;
                su->on_event = on_event; su->period_ms = per; su->sock = -1;
                su->last_ok_ms = now_ms();
                // store the space-separated param name list
                int o = 0;
                for (int i = 0; i < np && o < (int)sizeof(su->params) - ADBP_MAXNAME - 2; i++)
                    o += snprintf(su->params + o, sizeof(su->params) - o, "%s%s", i ? " " : "", names[i]);
                ESP_LOGI(TAG, "subscribe from %s:%ld per=%ldms onEvent=%d np=%d",
                         pip, pport, per, on_event, np);
                // The subscription mode decides which dropout path the EFB is
                // exposed to (spec 2026-08-13) — record it where /log can see it.
                logln("[adbp] subscribe %s per=%lums pubport=%ld np=%d from %s",
                      on_event ? "OnEvent" : "periodic", (unsigned long)per, pport, np, pip);
            } else logln("[adbp] no free subscription slot: %d EFBs already subscribed", MAX_SUBS);
        }
        build_params(body, sizeof B->body, names, np, &miss);
        adbp_wrap_resp(resp, sizeof B->resp, rmethod, 0, body);
        data_sent = true;
    } else if (is_unsub) {
        long pport = adbp_tag_num(req, "publishport", 0);
        for (int i = 0; i < MAX_SUBS; i++)
            if (s_subs[i].active && s_subs[i].ip == peer_ip && (pport == 0 || s_subs[i].port == pport)) {
                if (s_subs[i].sock >= 0) close(s_subs[i].sock);
                memset(&s_subs[i], 0, sizeof s_subs[i]); s_subs[i].sock = -1;
            }
        adbp_wrap_resp(resp, sizeof B->resp, rmethod, 0, "<parameters></parameters>");
    } else {
        adbp_wrap_resp(resp, sizeof B->resp, "UnknownMethod", 2, "<parameters></parameters>");
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
    // Non-blocking connect bounded by select(): lwIP ignores SO_SNDTIMEO for
    // connect() and would sit through every SYN retry to an EFB that is gone.
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(s, (struct sockaddr *)&a, sizeof a);
    if (rc != 0 && errno == EINPROGRESS) {
        fd_set w; FD_ZERO(&w); FD_SET(s, &w);
        struct timeval ct = { .tv_sec = 0, .tv_usec = PUSH_CONNECT_MS * 1000 };
        int err = 0; socklen_t el = sizeof err;
        if (select(s + 1, NULL, &w, NULL, &ct) == 1 &&
            getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) rc = 0;
    }
    if (rc != 0) { close(s); return -1; }
    fcntl(s, F_SETFL, fl);   // back to blocking; sends stay bounded by SO_SNDTIMEO
    int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return s;
}

static void push_subs(void) {
    pos_state_t p; pos_get(&p);
    uint32_t cur_fix = p.last_fix_ms, now = now_ms();
    char *body = B->pbody, *msg = B->msg;

    adbp_pstate_t st = adbp_classify(p.valid, p.last_fix_ms, now,
                                     CFG->stale_ms, CFG->dr_hold_ms);
    // One line per transition so a post-flight /log (or the flash log) shows
    // exactly when the EFB was moved between fresh / dead-reckoned / NCD.
    static adbp_pstate_t s_prev_state = ADBP_POS_NCD;
    if (st != s_prev_state) {
        static const char *nm[] = { "fresh", "dr", "ncd" };
        logln("[adbp] pos %s -> %s (fix age %lus)", nm[s_prev_state], nm[st],
              (unsigned long)((now - p.last_fix_ms) / 1000));
        s_prev_state = st;
    }

    for (int i = 0; i < MAX_SUBS; i++) {
        sub_t *su = &s_subs[i];
        if (!su->active) continue;
        bool due = adbp_push_due(su->on_event, cur_fix, su->last_fix_seen,
                                 now, su->last_push_ms, su->period_ms,
                                 st, su->last_state);
        if (!due) continue;

        if (su->sock < 0) {
            if (su->fails >= PUSH_FAILS_SLOW && now - su->last_push_ms < PUSH_RETRY_SLOW_MS)
                continue;
            su->sock = connect_push(su);
            if (su->sock < 0) {
                if (su->fails++ == 0) {
                    char ip[16]; ip_str(su->ip, ip, sizeof ip);
                    logln("[adbp] EFB %s:%u unreachable, keeping its subscription", ip, su->port);
                }
                su->last_push_ms = now;
                continue;
            }
            if (su->fails) {
                char ip[16]; ip_str(su->ip, ip, sizeof ip);
                logln("[adbp] EFB %s:%u reachable again after %lu failed connects", ip, su->port,
                      (unsigned long)su->fails);
                su->fails = 0;
            }
            su->was_connected = true; su->push_count = 0;
        }

        // rebuild the param name array from the stored list
        char names[ADBP_MAXPARAMS][ADBP_MAXNAME]; int np = 0;
        char tmp[1400]; strlcpy(tmp, su->params, sizeof tmp);
        for (char *tok = strtok(tmp, " "); tok && np < ADBP_MAXPARAMS; tok = strtok(NULL, " "))
            strlcpy(names[np++], tok, ADBP_MAXNAME);
        bool miss;
        build_params(body, sizeof B->pbody, names, np, &miss);

        const char *method = su->on_event ? "onEventAvionicParameters" : "publishAvionicParameters";
        bool with_prolog = CFG->frame_prolog_each || su->push_count == 0;
        int mlen = adbp_wrap_push(msg, sizeof B->msg, method, body, with_prolog, CFG);
        // delimiter
        char delim[3] = {0}; int dl = 0;
        switch (CFG->frame_delim) { case 1: delim[0]='\r'; delim[1]='\n'; dl=2; break;
                                    case 2: delim[0]='\n'; dl=1; break; case 3: delim[0]=0; dl=1; break; }

        int w = send(su->sock, msg, mlen, 0);
        if (w <= 0) { close(su->sock); su->sock = -1; su->was_connected = false; }
        else { if (dl) send(su->sock, delim, dl, 0); su->push_count++; s_push_seq++; su->last_ok_ms = now; }

        su->last_push_ms = now; su->last_fix_seen = cur_fix; su->last_state = st;
    }
}

static void adbp_task(void *arg) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    // The AID's own address, not INADDR_ANY: ADBP is for the EFB on the SoftAP
    // or the cable. On the cabin uplink anyone could read ownship or take all
    // MAX_SUBS slots (they never expire on their own).
    uint32_t mine = netcore_downstream_ip();
    a.sin_family = AF_INET; a.sin_addr.s_addr = mine ? mine : htonl(INADDR_ANY);
    a.sin_port = htons(CFG->adbp_port);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0) { ESP_LOGE(TAG, "bind :%u failed", CFG->adbp_port); vTaskDelete(NULL); return; }
    listen(ls, 4);
    ESP_LOGI(TAG, "[ADBP] server on :%u", CFG->adbp_port);

    sup_watch(SUP_ADBP, 30000);
    for (;;) {
        sup_beat(SUP_ADBP);
        fd_set r; FD_ZERO(&r); FD_SET(ls, &r);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };   // 200 ms -> ~5 Hz push cadence
        if (select(ls + 1, &r, NULL, NULL, &tv) > 0 && FD_ISSET(ls, &r)) {
            struct sockaddr_in ca; socklen_t cl = sizeof ca;
            int cs = accept(ls, (struct sockaddr *)&ca, &cl);
            if (cs >= 0) handle_request(cs, ca.sin_addr.s_addr);   // bound to the AID IP: AP/cable only
        }
        push_subs();
    }
}

bool adbp_feeding(void) {
    if (!s_subs) return false;
    uint32_t now = now_ms();
    for (int i = 0; i < MAX_SUBS; i++)
        if (s_subs[i].active && s_subs[i].push_count > 0 && now - s_subs[i].last_push_ms < 5000)
            return true;
    return false;
}

uint32_t adbp_push_seq(void) { return s_push_seq; }

void adbp_start(const aidlink_cfg_t *cfg) {
    CFG = cfg;
    s_subs = heap_caps_calloc(MAX_SUBS, sizeof *s_subs, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_subs) s_subs = calloc(MAX_SUBS, sizeof *s_subs);
    B = heap_caps_malloc(sizeof *B, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!B) B = malloc(sizeof *B);
    if (!s_subs || !B) { ESP_LOGE(TAG, "no RAM for ADBP buffers"); return; }
    for (int i = 0; i < MAX_SUBS; i++) s_subs[i].sock = -1;
    xTaskCreate(adbp_task, "adbp", 8192, NULL, 5, NULL);
}
