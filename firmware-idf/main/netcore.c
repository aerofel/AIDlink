// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "netcore.h"
#include "dnsfwd.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "dhcpserver/dhcpserver.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "net";
static esp_netif_t *s_sta, *s_ap;

static bool s_sta_up;
static uint8_t s_sta_ip[4];
static bool s_have_ssid;          // an uplink SSID is configured
static volatile bool s_no_reconnect;   // suppress auto-reconnect (during a scan)

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

bool netcore_sta_ipinfo(char *ip, char *gw, char *mask, char *dns) {
    ip[0] = gw[0] = mask[0] = dns[0] = 0;
    if (!s_sta_up || !s_sta) return false;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_sta, &info) == ESP_OK) {
        snprintf(ip, 16, IPSTR, IP2STR(&info.ip));
        snprintf(gw, 16, IPSTR, IP2STR(&info.gw));
        snprintf(mask, 16, IPSTR, IP2STR(&info.netmask));
    }
    esp_netif_dns_info_t d;
    if (esp_netif_get_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &d) == ESP_OK)
        snprintf(dns, 16, IPSTR, IP2STR(&d.ip.u_addr.ip4));
    return true;
}

// Scan uplink networks. The STA is put into a scannable state first: pause
// auto-reconnect and stop any in-progress connection attempt (the driver rejects
// a scan while "STA is connecting"). Returns 0 on success; fills *count.
int netcore_scan(wifi_ap_record_t *recs, uint16_t max, uint16_t *count) {
    *count = 0;
    s_no_reconnect = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(120));   // let the connecting state clear
    wifi_scan_config_t sc = { .show_hidden = true };
    esp_err_t e = esp_wifi_scan_start(&sc, true);   // blocking
    if (e == ESP_OK) {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        if (n > max) n = max;
        if (n) esp_wifi_scan_get_ap_records(&n, recs);
        *count = n;
    } else {
        ESP_LOGW(TAG, "scan_start failed: %s", esp_err_to_name(e));
    }
    s_no_reconnect = false;
    if (s_have_ssid) esp_wifi_connect();   // resume connecting to the uplink
    return e == ESP_OK ? 0 : -1;
}

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_ssid && !s_no_reconnect) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_up = false;
        if (s_have_ssid && !s_no_reconnect) esp_wifi_connect();
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

// Parse a dotted-quad string to a network-order IPv4 addr (0 on failure).
static uint32_t ip4_of(const char *s) {
    ip4_addr_t a;
    return ip4addr_aton(s, &a) ? a.addr : 0;
}

// Configure SoftAP IP, DHCP pool, and DNS offer (AP IP -> our forwarder).
static void configure_ap_netif(const aidlink_cfg_t *c) {
    esp_netif_dhcps_stop(s_ap);

    uint32_t ap_addr = ip4_of(c->ap_ip);
    uint32_t ap_mask = ip4_of(c->ap_mask);
    uint32_t lease_start = ip4_of(c->ap_lease);
    if (!ap_addr) ap_addr = ip4_of("172.20.1.1");
    if (!ap_mask) ap_mask = ip4_of("255.255.255.192");
    if (!lease_start) lease_start = (ap_addr & 0x00FFFFFF) | (((ap_addr >> 24) + 1) << 24);

    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = ap_addr; ip.gw.addr = ap_addr; ip.netmask.addr = ap_mask;
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap, &ip));

    // DHCP lease pool: from ap_lease for ap_dhcp_count addresses.
    int count = c->ap_dhcp_count ? c->ap_dhcp_count : 1;
    uint8_t last = (lease_start >> 24) & 0xFF;
    int end_last = last + count - 1; if (end_last > 254) end_last = 254;
    dhcps_lease_t lease = {0};
    lease.enable = true;
    lease.start_ip.addr = lease_start;
    lease.end_ip.addr = (lease_start & 0x00FFFFFF) | ((uint32_t)end_last << 24);
    esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS, &lease, sizeof(lease));
    uint32_t lease_min = c->ap_lease_min ? c->ap_lease_min : 120;
    esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_IP_ADDRESS_LEASE_TIME, &lease_min, sizeof(lease_min));

    // Offer the AP's own IP as the DNS server; the forwarder relays to the uplink.
    esp_netif_dns_info_t di = {0};
    di.ip.type = ESP_IPADDR_TYPE_V4;
    di.ip.u_addr.ip4.addr = ap_addr;
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
    s_have_ssid = c->sta_ssid[0] != 0;
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
    ESP_LOGI(TAG, "[AP] %s IP=%s mask=%s DNS->self", c->ap_ssid, c->ap_ip, c->ap_mask);

    dnsfwd_start(s_sta, c->ap_client_dns);
    return s_sta;
}
