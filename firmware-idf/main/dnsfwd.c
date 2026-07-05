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
                    p->used = false;
                }
            }
        }

        for (int i = 0; i < DNSFWD_SLOTS; i++)
            if (s_pend[i].used && now - s_pend[i].t0 > 3000) s_pend[i].used = false;
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
