// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "netcore.h"
#include "dnsfwd.h"
#include "log.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "dhcpserver/dhcpserver.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// On USB-capable targets (S3) we front the Wi-Fi AP and the USB-NCM port with an
// L2 bridge that owns the AID IP/DHCP/NAPT, so both see the AID at 172.20.1.1.
#if CONFIG_SOC_USB_OTG_SUPPORTED
#define AIDLINK_BRIDGE 1
#include "esp_netif_br_glue.h"
#endif

static const char *TAG = "net";
static esp_netif_t *s_sta, *s_ap;
#if AIDLINK_BRIDGE
static esp_netif_t *s_br;   // L2 bridge (Wi-Fi AP + USB-NCM), holds the AID IP
#endif

static bool s_sta_up;
static uint8_t s_sta_ip[4];
static bool s_have_ssid;          // an uplink SSID is configured
static volatile bool s_no_reconnect;   // suppress auto-reconnect (during a scan)
static volatile bool s_scanning;       // a Wi-Fi scan is in progress

esp_netif_t *netcore_sta_netif(void) { return s_sta; }
esp_netif_t *netcore_ap_netif(void) { return s_ap; }

esp_netif_t *netcore_bridge_netif(void) {
#if AIDLINK_BRIDGE
    return s_br;
#else
    return NULL;
#endif
}

// The netif that owns the AID IP, DHCP pool and NAPT: the bridge on the S3,
// else the SoftAP. Used for NAPT and for enumerating DHCP leases (clients list).
esp_netif_t *netcore_downstream_netif(void) {
#if AIDLINK_BRIDGE
    return s_br;
#else
    return s_ap;
#endif
}

bool netcore_sta_up(uint8_t ip4_out[4]) {
    if (s_sta_up && ip4_out) memcpy(ip4_out, s_sta_ip, 4);
    return s_sta_up;
}

int netcore_ap_client_count(void) {
    wifi_sta_list_t list;
    return esp_wifi_ap_get_sta_list(&list) == ESP_OK ? (int)list.num : 0;
}

bool netcore_has_ssid(void) { return s_have_ssid; }

int netcore_sta_rssi(void) {
    if (!s_sta_up) return 0;
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
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
    s_scanning = true;
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
    s_scanning = false;
    if (s_have_ssid) esp_wifi_connect();   // resume connecting to the uplink
    return e == ESP_OK ? 0 : -1;
}

bool netcore_scanning(void) { return s_scanning; }

// ---- internet reachability probe ------------------------------------------
// An associated uplink says nothing about actual internet (walled gardens).
// Probe with the cheapest possible exchange: a bare TCP handshake to a public
// DNS server (SYN / SYN-ACK / RST, ~200 bytes, no payload, no DNS query) —
// onboard data is metered. 30 s cadence while up, 15 s retry while down,
// prompt probe when the STA gets an IP.
// KNOWN LIMITATION (accepted, 2026-07-08): captive portals typically pass or
// transparently answer port 53 pre-auth, so this can show "internet" behind
// an unauthenticated hotspot. The content-validated alternative (HTTP
// generate_204, tri-state captive detection) was proposed and declined —
// keep the cheap handshake unless that decision changes.
static volatile bool s_inet;
static volatile bool s_inet_probe_now;

static bool tcp_probe(const char *ip, uint16_t port, int timeout_ms) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = inet_addr(ip);
    int fl = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, fl | O_NONBLOCK);
    bool ok = false;
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) ok = true;
    else if (errno == EINPROGRESS) {
        fd_set wf; FD_ZERO(&wf); FD_SET(s, &wf);
        struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
        if (select(s + 1, NULL, &wf, NULL, &tv) == 1) {
            int err = 0; socklen_t l = sizeof err;
            getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &l);
            ok = (err == 0);
        }
    }
    close(s);
    return ok;
}

static void inet_task(void *arg) {
    (void)arg;
    int alt = 0;                 // alternate 1.1.1.1 / 8.8.8.8 across failures
    uint32_t wait_ms = 3000;     // first probe shortly after boot
    for (;;) {
        for (uint32_t w = 0; w < wait_ms && !s_inet_probe_now; w += 500)
            vTaskDelay(pdMS_TO_TICKS(500));
        s_inet_probe_now = false;
        if (!s_sta_up) { s_inet = false; wait_ms = 5000; continue; }
        bool ok = tcp_probe(alt ? "8.8.8.8" : "1.1.1.1", 53, 3000);
        if (!ok) alt ^= 1;
        if (ok != s_inet) logln("internet %s", ok ? "reachable" : "unreachable");
        s_inet = ok;
        wait_ms = ok ? 30000 : 15000;
    }
}

bool netcore_inet_up(void) { return s_inet; }

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_ssid && !s_no_reconnect) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_up = false;
        s_inet = false;
        if (s_have_ssid && !s_no_reconnect) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = data;
        logln("AP client joined %02X:%02X:%02X:%02X:%02X:%02X",
              e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = data;
        logln("AP client left  %02X:%02X:%02X:%02X:%02X:%02X (reason %d)",
              e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], e->reason);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_sta_ip[0] = esp_ip4_addr1_16(&e->ip_info.ip);
        s_sta_ip[1] = esp_ip4_addr2_16(&e->ip_info.ip);
        s_sta_ip[2] = esp_ip4_addr3_16(&e->ip_info.ip);
        s_sta_ip[3] = esp_ip4_addr4_16(&e->ip_info.ip);
        s_sta_up = true;
        s_inet_probe_now = true;   // uplink just came up — probe internet promptly
        ESP_LOGI(TAG, "[STA] up IP=" IPSTR, IP2STR(&e->ip_info.ip));
    }
}

// Parse a dotted-quad string to a network-order IPv4 addr (0 on failure).
static uint32_t ip4_of(const char *s) {
    ip4_addr_t a;
    return ip4addr_aton(s, &a) ? a.addr : 0;
}

// Configure the downstream netif's IP, DHCP pool, and DNS offer (its own IP ->
// our forwarder). Works on either the SoftAP or the bridge netif.
static void configure_dhcp(esp_netif_t *nif, const aidlink_cfg_t *c) {
    esp_netif_dhcps_stop(nif);

    uint32_t ap_addr = ip4_of(c->ap_ip);
    uint32_t ap_mask = ip4_of(c->ap_mask);
    uint32_t lease_start = ip4_of(c->ap_lease);
    if (!ap_addr) ap_addr = ip4_of("172.20.1.1");
    if (!ap_mask) ap_mask = ip4_of("255.255.255.192");
    if (!lease_start) lease_start = (ap_addr & 0x00FFFFFF) | (((ap_addr >> 24) + 1) << 24);

    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = ap_addr; ip.gw.addr = ap_addr; ip.netmask.addr = ap_mask;
    ESP_ERROR_CHECK(esp_netif_set_ip_info(nif, &ip));

    // DHCP lease pool: from ap_lease for ap_dhcp_count addresses.
    int count = c->ap_dhcp_count ? c->ap_dhcp_count : 1;
    uint8_t last = (lease_start >> 24) & 0xFF;
    int end_last = last + count - 1; if (end_last > 254) end_last = 254;
    dhcps_lease_t lease = {0};
    lease.enable = true;
    lease.start_ip.addr = lease_start;
    lease.end_ip.addr = (lease_start & 0x00FFFFFF) | ((uint32_t)end_last << 24);
    esp_netif_dhcps_option(nif, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS, &lease, sizeof(lease));
    uint32_t lease_min = c->ap_lease_min ? c->ap_lease_min : 120;
    esp_netif_dhcps_option(nif, ESP_NETIF_OP_SET, ESP_NETIF_IP_ADDRESS_LEASE_TIME, &lease_min, sizeof(lease_min));

    // Offer our own IP as the DNS server; the forwarder relays to the uplink.
    esp_netif_dns_info_t di = {0};
    di.ip.type = ESP_IPADDR_TYPE_V4;
    di.ip.u_addr.ip4.addr = ap_addr;
    esp_netif_set_dns_info(nif, ESP_NETIF_DNS_MAIN, &di);
    dhcps_offer_t offer = OFFER_DNS;
    esp_netif_dhcps_option(nif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));

    esp_err_t e = esp_netif_dhcps_start(nif);
    esp_netif_ip_info_t got = {0};
    esp_netif_get_ip_info(nif, &got);
    ESP_LOGI(TAG, "[NET] DHCP on %s: %s IP=" IPSTR " pool " IPSTR "+%d",
             esp_netif_get_desc(nif), e == ESP_OK ? "started" : esp_err_to_name(e),
             IP2STR(&got.ip), IP2STR(&lease.start_ip), count);
}

#if AIDLINK_BRIDGE
// Build the L2 bridge that fronts the Wi-Fi AP (added here) and the USB-NCM port
// (added later by usb_ncm_start). The bridge owns the AID IP/DHCP; NAPT is
// enabled on it by the caller. s_ap must already be a bridged AP port (created
// with flags=AUTOUP, ip_info=NULL). Returns with the bridge started.
static void build_bridge(const aidlink_cfg_t *c) {
    uint32_t ap_addr = ip4_of(c->ap_ip);
    uint32_t ap_mask = ip4_of(c->ap_mask);
    if (!ap_addr) ap_addr = ip4_of("172.20.1.1");
    if (!ap_mask) ap_mask = ip4_of("255.255.255.192");
    static esp_netif_ip_info_t brip;   // must outlive esp_netif_new
    memset(&brip, 0, sizeof brip);
    brip.ip.addr = ap_addr; brip.gw.addr = ap_addr; brip.netmask.addr = ap_mask;

    esp_netif_inherent_config_t brc = ESP_NETIF_INHERENT_DEFAULT_BR_DHCPS();
    brc.ip_info = &brip;                                  // override the 192.168.4.1 default
    static bridgeif_config_t brinfo = {                  // must outlive the netif
        .max_fdb_dyn_entries = 16, .max_fdb_sta_entries = 4, .max_ports = 4,
    };
    brc.bridge_info = &brinfo;
    esp_read_mac(brc.mac, ESP_MAC_WIFI_SOFTAP);          // bridge MAC = device (AP) MAC
    esp_netif_config_t brcfg = { .base = &brc, .stack = ESP_NETIF_NETSTACK_DEFAULT_BR };
    s_br = esp_netif_new(&brcfg);

    esp_netif_br_glue_handle_t glue = esp_netif_br_glue_new();
    ESP_ERROR_CHECK(esp_netif_br_glue_add_wifi_port(glue, s_ap));
    ESP_ERROR_CHECK(esp_netif_attach(s_br, glue));
    // The glue owns the bridge lifecycle: on WIFI_EVENT_AP_START it starts the
    // bridge, adds the AP port, and (DHCP_SERVER flag) starts DHCP. We apply our
    // custom pool afterwards in netcore_start, once the bridge is up.
}
#endif

esp_netif_t *netcore_start(const aidlink_cfg_t *c) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    s_sta = esp_netif_create_default_wifi_sta();
#if AIDLINK_BRIDGE
    // AP as a bridged L2 port: no IP of its own, keep AUTOUP so it comes up to be
    // bridged. The bridge (built below) holds the AID IP + DHCP + NAPT.
    esp_netif_inherent_config_t apc = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
    apc.flags = ESP_NETIF_FLAG_AUTOUP;
    apc.ip_info = NULL;
    s_ap = esp_netif_create_wifi(WIFI_IF_AP, &apc);
    ESP_ERROR_CHECK(esp_wifi_set_default_wifi_ap_handlers());
#else
    s_ap = esp_netif_create_default_wifi_ap();
#endif

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

#if AIDLINK_BRIDGE
    build_bridge(c);        // creates s_br, bridges the AP port, starts DHCP on the bridge
#else
    configure_dhcp(s_ap, c);
#endif

    ESP_ERROR_CHECK(esp_wifi_start());

#if AIDLINK_BRIDGE
    // The glue starts the bridge netif on WIFI_EVENT_AP_START but only brings its
    // link "up" on the first Wi-Fi association (WIFI_EVENT_AP_STACONNECTED). We
    // need the bridge up unconditionally — a USB client alone, with no Wi-Fi
    // client, must still get DHCP/NAPT — so we connect it ourselves once the glue
    // has created it (AP_START), then apply our DHCP pool. action_connected only
    // brings the link up (no netif_add), so it won't collide with the glue.
    vTaskDelay(pdMS_TO_TICKS(300));                      // let AP_START reach the glue
    esp_netif_action_connected(s_br, NULL, 0, NULL);     // bring the bridge link up
    for (int i = 0; i < 20 && !esp_netif_is_netif_up(s_br); i++) vTaskDelay(pdMS_TO_TICKS(50));
    configure_dhcp(s_br, c);
#endif

    esp_netif_t *down = netcore_downstream_netif();
    if (c->napt_enable) {
        esp_err_t e = esp_netif_napt_enable(down);
        ESP_LOGI(TAG, "[NET] NAPT %s", e == ESP_OK ? "ON" : "FAILED");
    }
    ESP_LOGI(TAG, "[NET] %s AID IP=%s mask=%s DNS->self%s", c->ap_ssid, c->ap_ip, c->ap_mask,
             netcore_bridge_netif() ? " (bridged AP+USB)" : "");

    dnsfwd_start(s_sta, c->ap_client_dns);
    xTaskCreate(inet_task, "inetprobe", 3072, NULL, 2, NULL);
    return s_sta;
}
