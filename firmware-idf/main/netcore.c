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
#include "esp_http_client.h"
#include "esp_timer.h"
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
// Internet reachability = an HTTP generate_204 GET that must return EXACTLY 204.
// This tells real internet apart from a captive-portal intercept (which answers
// 200 + HTML or a 3xx redirect) and from no uplink (timeout). The earlier probe
// was a bare TCP handshake to a public-DNS IP (1.1.1.1:53) — but this Viasat
// satellite walled garden BLOCKS direct-IP-to-public-DNS on 53 *and* 443 while
// passing real hostname HTTPS, so the old probe reported "no internet" when there
// was internet (false red cloud). Measured cost on the metered link: ~127 B of
// response headers + framing ≈ 0.8 KB/probe; at the 60 s cadence below that is
// ~1.2 MB/day. Port 80 (no TLS) deliberately — HTTPS would add a ~2.5 KB
// handshake per probe. The body is never read: a real 204 has none, and a
// captive 200's HTML must not be downloaded on metered data.
#define INET_PROBE_URL "http://connectivitycheck.gstatic.com/generate_204"
static volatile bool s_inet;
static volatile bool s_inet_probe_now;
static volatile inet_state_t s_inet_state = INET_NO_UPLINK;

// Provider service hint (see netcore.h). Expires so a stalled position source
// cannot pin a stale "service off" verdict forever.
#define SVC_HINT_TTL_MS 120000
static volatile svc_tri_t s_svc = SVC_UNKNOWN;
static volatile uint32_t  s_svc_at_ms;
static char s_svc_reason[32];

void netcore_service_hint(svc_tri_t available, const char *reason) {
    s_svc = available;
    s_svc_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (reason) strlcpy(s_svc_reason, reason, sizeof s_svc_reason);
    else s_svc_reason[0] = 0;
}

static svc_tri_t svc_hint_fresh(void) {
    if (s_svc == SVC_UNKNOWN) return SVC_UNKNOWN;
    uint32_t age = (uint32_t)(esp_timer_get_time() / 1000) - s_svc_at_ms;
    return age > SVC_HINT_TTL_MS ? SVC_UNKNOWN : s_svc;
}

inet_state_t netcore_inet_state(void) { return s_inet_state; }

static const char *state_str(inet_state_t s) {
    switch (s) {
        case INET_OK:          return "ok";
        case INET_PORTAL:      return "portal";
        case INET_SERVICE_OFF: return "service_off";
        case INET_DOWN:        return "down";
        default:               return "no_uplink";
    }
}

const char *netcore_inet_state_str(void) { return state_str(s_inet_state); }

// Probe outcome. The distinction that matters is between "something answered
// but it wasn't a 204" (a captive portal is intercepting -> the user can fix
// this by signing in) and "nothing answered at all" (the link itself is down).
typedef enum { PROBE_204, PROBE_INTERCEPT, PROBE_NOLINK } probe_res_t;

static probe_res_t http204_probe(int timeout_ms) {
    esp_http_client_config_t c = {
        .url = INET_PROBE_URL,
        .timeout_ms = timeout_ms,
        .disable_auto_redirect = true,   // a captive 3xx must read as "not 204", not be followed
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return PROBE_NOLINK;
    esp_http_client_set_header(h, "User-Agent", "aidlink");
    probe_res_t res = PROBE_NOLINK;
    if (esp_http_client_open(h, 0) == ESP_OK) {          // GET, no request body
        // read status + headers only; the body is intentionally left unread
        if (esp_http_client_fetch_headers(h) >= 0) {
            int status = esp_http_client_get_status_code(h);
            // Exactly 204 is real internet. Anything else that still produced a
            // status line came from an intercepting portal (200 + HTML, a 3xx
            // redirect, or 511 Network Authentication Required).
            res = (status == 204) ? PROBE_204 : PROBE_INTERCEPT;
        }
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return res;
}

static void inet_task(void *arg) {
    (void)arg;
    uint32_t wait_ms = 3000;     // first probe shortly after boot
    for (;;) {
        for (uint32_t w = 0; w < wait_ms && !s_inet_probe_now; w += 500)
            vTaskDelay(pdMS_TO_TICKS(500));
        s_inet_probe_now = false;
        if (!s_sta_up) {
            if (s_inet_state != INET_NO_UPLINK) logln("internet: no uplink");
            s_inet_state = INET_NO_UPLINK; s_inet = false; wait_ms = 60000; continue;
        }
        probe_res_t pr = http204_probe(12000);   // 12 s tolerates satellite RTT (measured 1-10 s)

        inet_state_t st;
        if (pr == PROBE_204)            st = INET_OK;
        else if (pr == PROBE_INTERCEPT) st = INET_PORTAL;        // sign-in needed
        else if (svc_hint_fresh() == SVC_NO) st = INET_SERVICE_OFF;
        else                            st = INET_DOWN;          // link itself is dead

        if (st != s_inet_state) {
            if (st == INET_SERVICE_OFF && s_svc_reason[0])
                logln("internet: service_off (%s)", s_svc_reason);
            else
                logln("internet: %s", state_str(st));
        }
        s_inet_state = st;
        s_inet = (st == INET_OK);
        wait_ms = 60000;         // every 60 s (~0.8 KB/probe; the plan is time-based, not metered)
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

    // Drop the 802.11b rates on the AP. Broadcast, multicast and management
    // frames go out at the lowest supported basic rate: with 11b enabled that is
    // 1 Mbit/s, roughly 6x the airtime of the 6 Mbit/s OFDM basic rate for the
    // same frame. In a cabin we share one congested 2.4 GHz channel with the
    // uplink AP and every other passenger, AND the AP+STA are the same radio, so
    // that airtime is the scarce resource -- measured as a jittery client hop
    // (3.5/15.2/113.5 ms) against 1.1/1.7/3.4 ms over the USB cable. Every EFB,
    // phone and laptop is 802.11n or better; only a genuinely 11b-only client
    // would be excluded, and none exists on this aircraft.
    esp_err_t rate_err = esp_wifi_config_11b_rate(WIFI_IF_AP, true);
    ESP_LOGI(TAG, "[NET] AP 11b rates %s",
             rate_err == ESP_OK ? "disabled" : esp_err_to_name(rate_err));

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
