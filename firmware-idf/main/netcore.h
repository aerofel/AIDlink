// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Network core: Wi-Fi STA uplink + SoftAP downstream with DHCP, a DNS offer of
// the AP's own IP (served by the DNS forwarder), and NAPT to the uplink.
#pragma once
#include "config.h"
#include "esp_netif.h"
#include "esp_wifi.h"

// Brings up Wi-Fi APSTA, connects STA, configures the SoftAP (IP, DHCP pool,
// DNS offer), enables NAPT, and starts the DNS forwarder. Returns the STA netif.
esp_netif_t *netcore_start(const aidlink_cfg_t *c);

esp_netif_t *netcore_sta_netif(void);
esp_netif_t *netcore_ap_netif(void);

// The downstream netif that owns the AID IP, DHCP pool and NAPT — and that
// serves DHCP leases for the clients list. On the S3 this is the L2 bridge
// (Wi-Fi AP + USB-NCM share it); on the classic ESP32 it is the SoftAP itself.
// Never NULL after netcore_start().
esp_netif_t *netcore_downstream_netif(void);

// The L2 bridge netif (S3 only), or NULL when bridging isn't built. USB-NCM
// attaches itself to this as a bridge port.
esp_netif_t *netcore_bridge_netif(void);

// True once the STA has an IP (uplink usable). Fills ip4 (4 bytes) when up.
bool netcore_sta_up(uint8_t ip4_out[4]);

// Live STA IP config as dotted-quad strings (each buf >=16). Returns true when
// the STA is up; on DHCP this reflects the values assigned by the uplink.
bool netcore_sta_ipinfo(char *ip, char *gw, char *mask, char *dns);
// Count of stations currently associated to the SoftAP.
int netcore_ap_client_count(void);

// True if an uplink SSID is configured (STA is actively trying to connect).
bool netcore_has_ssid(void);

// RSSI (dBm) of the connected uplink AP; 0 when the STA is not connected.
int netcore_sta_rssi(void);

// True when the last internet-reachability probe verified real internet.
// Equivalent to netcore_inet_state() == INET_OK; kept for callers that only
// need the red/green answer (display, LED).
bool netcore_inet_up(void);

// Why there is (or isn't) internet. A single red light cannot distinguish "you
// never signed in" from "the satellite is down", and those need opposite actions
// from the crew, so the probe result is kept as a state.
//
// Deliberately PROVIDER-NEUTRAL: three of these come from the generic
// captive-portal probe (a non-204 HTTP answer means something is intercepting,
// no answer at all means the link is dead), and only INET_SERVICE_OFF needs a
// hint from whoever understands the current provider's feed. Nothing in netcore
// knows what "Viasat" is, so a new provider only has to push the same hint.
typedef enum {
    INET_NO_UPLINK = 0,   // the STA is not associated to any uplink AP
    INET_OK,              // verified end-to-end (probe returned exactly 204)
    INET_PORTAL,          // something intercepted the probe -> sign-in required
    INET_SERVICE_OFF,     // the provider reports its service unavailable here
    INET_DOWN,            // associated, nothing intercepting, still no internet
} inet_state_t;

inet_state_t netcore_inet_state(void);
const char  *netcore_inet_state_str(void);   // stable slug for /status and logs

// Provider-neutral service hint. A position source that can see its provider's
// own service flags pushes them here; netcore never learns which provider it is.
// `reason` is a short provider-specific string for display/logging (may be NULL).
// Hints expire on their own, so a stalled source cannot pin a stale verdict.
typedef enum { SVC_UNKNOWN = 0, SVC_YES, SVC_NO } svc_tri_t;
void netcore_service_hint(svc_tri_t available, const char *reason);

// True while a Wi-Fi scan is in progress.
bool netcore_scanning(void);

// Scan uplink Wi-Fi networks into recs[max]; fills *count. Returns 0 on success.
// Coordinates with the STA connection state so the scan isn't rejected.
int netcore_scan(wifi_ap_record_t *recs, uint16_t max, uint16_t *count);
