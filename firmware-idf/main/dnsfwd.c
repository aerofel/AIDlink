// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// UDP :53 DNS relay task. Listens on all local IPs (so both the SoftAP gateway
// and the USB-NCM gateway are covered), forwards each query to the live uplink
// resolver with a NAT-style transaction-id remap, and returns the reply. The
// upstream is resolved per query, so DNS follows STA reconnects and never goes
// stale in a client's DHCP lease.
#include "dnsfwd.h"
#include <string.h>
#include "lwip/sockets.h"
#include "lwip/ip4_addr.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dnsfwd";
static void *s_sta;
static char  s_override[16];
static dnsfwd_pend_t s_pend[DNSFWD_SLOTS];
static uint16_t s_seq;

// Returns the upstream resolver as a network-order IPv4 addr, or 0 if none.
static uint32_t upstream_addr(void) {
    if (s_override[0]) {
        ip4_addr_t a;
        if (ip4addr_aton(s_override, &a)) return a.addr;
    }
    esp_netif_dns_info_t d;
    if (s_sta && esp_netif_get_dns_info((esp_netif_t *)s_sta, ESP_NETIF_DNS_MAIN, &d) == ESP_OK) {
        if (d.ip.u_addr.ip4.addr) return d.ip.u_addr.ip4.addr;
    }
    return 0;
}

// ---- reply cache -----------------------------------------------------------
// Keyed on the raw question section (name + QTYPE + QCLASS). Shared by every
// client, which is the whole point: each phone/tablet on the AP keeps its own
// OS cache, so today the same hostname costs a separate satellite round trip
// for each of them.
typedef struct {
    uint8_t  q[DNSFWD_CACHE_QMAX];
    uint8_t  resp[DNSFWD_CACHE_RMAX];
    uint16_t qlen, rlen;
    uint32_t expire_ms;
    bool     used;
} cache_ent_t;

static cache_ent_t s_cache[DNSFWD_CACHE_N];
static uint32_t s_cache_hits, s_cache_miss;

// Serve from cache into `out` (capacity `cap`), preserving the caller's txn id.
// Returns the reply length, or -1 on a miss.
static int cache_get(const uint8_t *q, int qlen, uint32_t now, uint8_t *out, int cap) {
    for (int i = 0; i < DNSFWD_CACHE_N; i++) {
        cache_ent_t *e = &s_cache[i];
        if (!e->used) continue;
        if ((int32_t)(now - e->expire_ms) >= 0) { e->used = false; continue; }  // expired
        if (!dnsfwd_question_eq(q, qlen, e->q, e->qlen)) continue;
        if (e->rlen > cap) return -1;
        memcpy(out, e->resp, e->rlen);
        s_cache_hits++;
        return e->rlen;
    }
    s_cache_miss++;
    return -1;
}

static void cache_put(const uint8_t *q, int qlen, const uint8_t *resp, int rlen, uint32_t now) {
    if (qlen <= 0 || qlen > DNSFWD_CACHE_QMAX || rlen <= 0 || rlen > DNSFWD_CACHE_RMAX) return;
    if (!dnsfwd_cacheable(resp, rlen)) return;

    int ttl = dnsfwd_min_ttl(resp, rlen);
    // A negative reply (NXDOMAIN/NODATA) carries no answer TTL; hold it for the
    // floor rather than not at all, so a burst of repeats still collapses.
    if (ttl < 0) ttl = DNSFWD_CACHE_MIN_S;
    if (ttl < DNSFWD_CACHE_MIN_S) ttl = DNSFWD_CACHE_MIN_S;
    if (ttl > DNSFWD_CACHE_MAX_S) ttl = DNSFWD_CACHE_MAX_S;

    // Reuse the matching entry, else a free one, else the one expiring soonest.
    int victim = -1;
    for (int i = 0; i < DNSFWD_CACHE_N; i++)
        if (s_cache[i].used && dnsfwd_question_eq(q, qlen, s_cache[i].q, s_cache[i].qlen)) { victim = i; break; }
    if (victim < 0)
        for (int i = 0; i < DNSFWD_CACHE_N; i++) if (!s_cache[i].used) { victim = i; break; }
    if (victim < 0) {
        victim = 0;
        for (int i = 1; i < DNSFWD_CACHE_N; i++)
            if ((int32_t)(s_cache[i].expire_ms - s_cache[victim].expire_ms) < 0) victim = i;
    }

    cache_ent_t *e = &s_cache[victim];
    memcpy(e->q, q, (size_t)qlen);
    memcpy(e->resp, resp, (size_t)rlen);
    e->qlen = (uint16_t)qlen;
    e->rlen = (uint16_t)rlen;
    e->expire_ms = now + (uint32_t)ttl * 1000u;
    e->used = true;
}

void dnsfwd_cache_stats(uint32_t *hits, uint32_t *misses) {
    if (hits) *hits = s_cache_hits;
    if (misses) *misses = s_cache_miss;
}

static void dnsfwd_task(void *arg) {
    int srv = socket(AF_INET, SOCK_DGRAM, 0);
    int up = socket(AF_INET, SOCK_DGRAM, 0);
    if (srv < 0 || up < 0) { ESP_LOGE(TAG, "socket() failed"); vTaskDelete(NULL); return; }
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_ANY); sa.sin_port = htons(53);
    if (bind(srv, (struct sockaddr *)&sa, sizeof sa) < 0) ESP_LOGE(TAG, "bind :53 failed");
    ESP_LOGI(TAG, "forwarder up on :53");

    static uint8_t buf[1400];
    for (;;) {
        fd_set r; FD_ZERO(&r); FD_SET(srv, &r); FD_SET(up, &r);
        int mx = (srv > up ? srv : up) + 1;
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int n = select(mx, &r, NULL, NULL, &tv);
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        if (n > 0 && FD_ISSET(srv, &r)) {
            struct sockaddr_in ca; socklen_t cl = sizeof ca;
            int len = recvfrom(srv, buf, sizeof buf, 0, (struct sockaddr *)&ca, &cl);

            // Answer the types the uplink can never usefully resolve right here,
            // before spending a slot or a satellite round trip on them. A stub
            // resolver fans out A + AAAA (+ HTTPS) per hostname, so on a page
            // with many hosts this removes the majority of upstream queries and
            // lets getaddrinfo() return as soon as the A record lands.
            int qt = dnsfwd_qtype(buf, len);
            if (dnsfwd_answer_locally(qt)) {
                int rlen = dnsfwd_make_nodata(buf, len, (int)sizeof buf);
                if (rlen > 0)
                    sendto(srv, buf, rlen, 0, (struct sockaddr *)&ca, cl);
                continue;
            }

            // Cache lookup. A hit costs ~1 ms instead of a satellite round trip
            // (measured 610-835 ms), and it is shared across every device on the
            // AP rather than living in one client's private OS cache.
            int qe = dnsfwd_question_end(buf, len);
            int qlen = qe > 12 ? qe - 12 : 0;
            if (qlen > 0 && qlen <= DNSFWD_CACHE_QMAX) {
                uint8_t hit[DNSFWD_CACHE_RMAX];
                int hlen = cache_get(buf + 12, qlen, now, hit, (int)sizeof hit);
                if (hlen > 0) {
                    hit[0] = buf[0]; hit[1] = buf[1];   // this client's txn id
                    sendto(srv, hit, hlen, 0, (struct sockaddr *)&ca, cl);
                    continue;
                }
            }

            uint32_t upa = upstream_addr();
            if (len >= 12 && upa) {
                int slot = -1;
                for (int i = 0; i < DNSFWD_SLOTS; i++) if (!s_pend[i].used) { slot = i; break; }
                if (slot < 0) {  // table full -> evict oldest
                    uint32_t best = 0; slot = 0;
                    for (int i = 0; i < DNSFWD_SLOTS; i++) {
                        uint32_t age = now - s_pend[i].t0;
                        if (age >= best) { best = age; slot = i; }
                    }
                }
                uint16_t sid = dnsfwd_make_sid(slot, &s_seq);
                dnsfwd_pend_t *p = &s_pend[slot];
                memcpy(p->cip, &ca.sin_addr.s_addr, 4);
                p->cport = ca.sin_port;
                p->oid = (buf[0] << 8) | buf[1];
                p->sid = sid; p->t0 = now; p->used = true;
                p->qlen = (uint16_t)((qlen > 0 && qlen <= DNSFWD_CACHE_QMAX) ? qlen : 0);
                if (p->qlen) memcpy(p->q, buf + 12, p->qlen);
                buf[0] = sid >> 8; buf[1] = sid & 0xFF;
                struct sockaddr_in ua = {0};
                ua.sin_family = AF_INET; ua.sin_addr.s_addr = upa; ua.sin_port = htons(53);
                sendto(up, buf, len, 0, (struct sockaddr *)&ua, sizeof ua);
            }
        }

        if (n > 0 && FD_ISSET(up, &r)) {
            int len = recvfrom(up, buf, sizeof buf, 0, NULL, NULL);
            if (len >= 12) {
                uint16_t sid = (buf[0] << 8) | buf[1];
                int slot = dnsfwd_slot_of(sid);
                dnsfwd_pend_t *p = &s_pend[slot];
                if (p->used && p->sid == sid) {
                    buf[0] = p->oid >> 8; buf[1] = p->oid & 0xFF;  // restore client's id
                    struct sockaddr_in ca = {0};
                    ca.sin_family = AF_INET;
                    memcpy(&ca.sin_addr.s_addr, p->cip, 4);
                    ca.sin_port = p->cport;
                    sendto(srv, buf, len, 0, (struct sockaddr *)&ca, sizeof ca);
                    if (p->qlen) cache_put(p->q, p->qlen, buf, len, now);
                    p->used = false;
                }
            }
        }

        for (int i = 0; i < DNSFWD_SLOTS; i++)
            if (s_pend[i].used && now - s_pend[i].t0 > DNSFWD_TIMEOUT_MS) s_pend[i].used = false;
    }
}

void dnsfwd_start(void *sta_netif, const char *client_dns_override) {
    s_sta = sta_netif;
    s_override[0] = 0;
    if (client_dns_override && client_dns_override[0])
        strncpy(s_override, client_dns_override, sizeof s_override - 1);
    for (int i = 0; i < DNSFWD_SLOTS; i++) s_pend[i].used = false;
    xTaskCreate(dnsfwd_task, "dnsfwd", 4096, NULL, 5, NULL);
}
