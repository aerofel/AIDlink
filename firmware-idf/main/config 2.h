// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Device configuration (persisted in NVS namespace "aidlink"). This header is
// intentionally free of ESP-IDF types so the pure helpers below can be built and
// unit-tested on the host with a plain C compiler.
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char    sta_ssid[33];      // uplink SSID we connect to
    char    sta_pass[65];      // uplink passphrase
    char    ap_ssid[33];       // our SoftAP SSID
    char    ap_pass[65];       // our SoftAP passphrase
    uint8_t ap_ip[4];          // SoftAP / gateway IP (e.g. 172.20.1.1)
    uint8_t ap_prefix;         // SoftAP subnet prefix length (e.g. 26)
    uint16_t ap_lease_min;     // DHCP lease time, minutes
    uint8_t ap_dhcp_count;     // DHCP pool size
    char    ap_client_dns[16]; // optional forced client DNS ("" = live uplink resolver)
    uint8_t usb_ip[4];         // USB-NCM gateway IP (S3 only, e.g. 172.20.2.1)
    uint8_t usb_prefix;        // USB-NCM subnet prefix length (e.g. 29)
    bool    napt_enable;       // master NAT switch
} aidlink_cfg_t;

// Pure helper (host-testable): host-order netmask for a CIDR prefix.
// prefix 26 -> 0xFFFFFFC0, prefix 29 -> 0xFFFFFFF8, prefix 0 -> 0.
uint32_t cfg_netmask_from_prefix(uint8_t prefix);

// Load config from NVS, seeding defaults for any missing key.
void cfg_load(aidlink_cfg_t *c);

// Persist config to NVS. Returns 0 on success, -1 on error.
int cfg_save(const aidlink_cfg_t *c);
