// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Network core: Wi-Fi STA uplink + SoftAP downstream with DHCP, a DNS offer of
// the AP's own IP (served by the DNS forwarder), and NAPT to the uplink.
#pragma once
#include "config.h"
#include "esp_netif.h"

// Brings up Wi-Fi APSTA, connects STA, configures the SoftAP (IP, DHCP pool,
// DNS offer), enables NAPT, and starts the DNS forwarder. Returns the STA netif.
esp_netif_t *netcore_start(const aidlink_cfg_t *c);

esp_netif_t *netcore_sta_netif(void);
esp_netif_t *netcore_ap_netif(void);

// True once the STA has an IP (uplink usable). Fills ip4 (4 bytes) when up.
bool netcore_sta_up(uint8_t ip4_out[4]);
// Count of stations currently associated to the SoftAP.
int netcore_ap_client_count(void);
