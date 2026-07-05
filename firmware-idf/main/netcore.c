// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "netcore.h"
#include "dnsfwd.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "dhcpserver/dhcpserver.h"

static const char *TAG = "net";
static esp_netif_t *s_sta, *s_ap;

// Build a network-order IPv4 addr (ESP32 is little-endian, lwIP addr is net-order).
static inline uint32_t mk_netaddr(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
}

static bool s_sta_up;
static uint8_t s_sta_ip[4];

esp_netif_t *netcore_sta_netif(void) { return s_sta; }
esp_netif_t *netcore_ap_netif(void) { return s_ap; }

bool netcore_sta_up(uint8_t ip4_out[4]) {
    if (s_sta_up && ip4_out) memcpy(ip4_out, s_sta_ip, 4);
    return s_sta_up;
}

int netcore_ap_client_count(void) {
    wifi_sta_list_t list;
    return esp_wifi_ap_get_sta_list(&list) == ESP_OK ? (int)list.num : 0;
}

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_up = false;
        ESP_LOGW(TAG, "[STA] disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_sta_ip[0] = esp_ip4_addr1_16(&e->ip_info.ip);
        s_sta_ip[1] = esp_ip4_addr2_16(&e->ip_info.ip);
        s_sta_ip[2] = esp_ip4_addr3_16(&e->ip_info.ip);
        s_sta_ip[3] = esp_ip4_addr4_16(&e->ip_info.ip);
        s_sta_up = true;
        ESP_LOGI(TAG, "[STA] up IP=" IPSTR, IP2STR(&e->ip_info.ip));
    }
}

// Configure SoftAP IP, DHCP pool, and DNS offer (AP IP -> our forwarder).
static void configure_ap_netif(const aidlink_cfg_t *c) {
    esp_netif_dhcps_stop(s_ap);

    esp_netif_ip_info_t ip = {0};
    esp_netif_set_ip4_addr(&ip.ip, c->ap_ip[0], c->ap_ip[1], c->ap_ip[2], c->ap_ip[3]);
    esp_netif_set_ip4_addr(&ip.gw, c->ap_ip[0], c->ap_ip[1], c->ap_ip[2], c->ap_ip[3]);
    uint32_t m = cfg_netmask_from_prefix(c->ap_prefix);
    esp_netif_set_ip4_addr(&ip.netmask, (m >> 24) & 0xFF, (m >> 16) & 0xFF, (m >> 8) & 0xFF, m & 0xFF);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap, &ip));

    // DHCP lease pool: start at ap_ip.(last+1), count = ap_dhcp_count
    int start_last = c->ap_ip[3] + 1;
    int end_last = start_last + (c->ap_dhcp_count ? c->ap_dhcp_count : 1) - 1;
    if (end_last > 254) end_last = 254;
    dhcps_lease_t lease = {0};
    lease.enable = true;
    lease.start_ip.addr = mk_netaddr(c->ap_ip[0], c->ap_ip[1], c->ap_ip[2], start_last);
    lease.end_ip.addr = mk_netaddr(c->ap_ip[0], c->ap_ip[1], c->ap_ip[2], end_last);
    esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS, &lease, sizeof(lease));
    uint32_t lease_min = c->ap_lease_min ? c->ap_lease_min : 120;
    esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_IP_ADDRESS_LEASE_TIME, &lease_min, sizeof(lease_min));

    // Offer the AP's own IP as the DNS server; the forwarder relays to the uplink.
    esp_netif_dns_info_t di = {0};
    di.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_ip4_addr(&di.ip.u_addr.ip4, c->ap_ip[0], c->ap_ip[1], c->ap_ip[2], c->ap_ip[3]);
    esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, &di);
    dhcps_offer_t offer = OFFER_DNS;
    esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));

    esp_netif_dhcps_start(s_ap);
}

esp_netif_t *netcore_start(const aidlink_cfg_t *c) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // STA config
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, c->sta_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, c->sta_pass, sizeof(sta.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));

    // AP config
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, c->ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(c->ap_ssid);
    strlcpy((char *)ap.ap.password, c->ap_pass, sizeof(ap.ap.password));
    ap.ap.max_connection = 8;
    ap.ap.authmode = strlen(c->ap_pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap.ap.channel = 0;  // follow STA
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    configure_ap_netif(c);

    ESP_ERROR_CHECK(esp_wifi_start());

    if (c->napt_enable) {
        esp_err_t e = esp_netif_napt_enable(s_ap);
        ESP_LOGI(TAG, "[AP] NAPT %s", e == ESP_OK ? "ON" : "FAILED");
    }
    ESP_LOGI(TAG, "[AP] %s IP=%d.%d.%d.%d/%d DNS->self",
             c->ap_ssid, c->ap_ip[0], c->ap_ip[1], c->ap_ip[2], c->ap_ip[3], c->ap_prefix);

    dnsfwd_start(s_sta, c->ap_client_dns);
    return s_sta;
}
