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
    // --- uplink (STA) ---
    char    sta_ssid[33];      // uplink SSID we connect to
    char    sta_pass[65];      // uplink passphrase
    bool    sta_dhcp;          // true = DHCP, false = static below
    char    sta_ip[16], sta_gw[16], sta_mask[16], sta_dns[16];  // static uplink
    // --- SoftAP downstream ---
    char    ap_ssid[33];       // our SoftAP SSID
    char    ap_pass[65];       // our SoftAP passphrase
    bool    ap_hidden;         // hide SSID beacon
    char    ap_ip[16];         // SoftAP / AID IP (e.g. "172.20.1.1")
    char    ap_mask[16];       // SoftAP netmask (e.g. "255.255.255.192")
    char    ap_lease[16];      // DHCP pool start IP (e.g. "172.20.1.2")
    uint16_t ap_lease_min;     // DHCP lease time, minutes
    int      ap_dhcp_count;    // DHCP pool size
    int      ap_channel;       // 0 = follow STA
    int      ap_max_clients;   // 1..10
    char    ap_client_dns[16]; // optional forced client DNS ("" = live uplink resolver)
    // --- USB-NCM downstream (S3) ---
    uint8_t usb_ip[4];         // USB-NCM gateway IP (S3 only, e.g. 172.20.2.1)
    uint8_t usb_prefix;        // USB-NCM subnet prefix length (e.g. 29)
    bool    napt_enable;       // master NAT switch
    // --- device / services ---
    char    dev_name[32];      // hostname -> mDNS <dev_name>.local
    uint16_t adbp_port;        // ADBP TCP port (default 24000)
    uint16_t ds_port;          // EFB DataStreamPort (default 51000)
    // ADBP push framing (ARINC-834 experiment): frame_len 0=full/1=method/2=omit,
    // frame_delim 0=none/1=CRLF/2=LF/3=NUL, frame_prolog_each = XML prolog every frame.
    int      frame_len, frame_delim; bool frame_prolog_each;
    char    api_ver[8];        // AID Web API version string (e.g. "3.1")
    char    ac_tail[12], ac_type[8];  // aircraft identity
    // --- position source ---
    int      src_type;         // 0=Viasat, 1=Panasonic, 2=custom
    char     vs_url[128];      // custom/test source URL
    uint32_t poll_ms, stale_ms;
    // --- emulator ---
    bool     sim_enable;
    double   sim_lat, sim_lon, sim_trk, sim_gs, sim_alt;
    // --- logging ---
    bool     log_enable;
    // --- auth (settings-page login; salted SHA-256, no plaintext) ---
    bool     auth_enable;
    char     auth_user[24];
    char     auth_hash[65];    // 64 hex chars + NUL
    char     auth_salt[24];
} aidlink_cfg_t;

// Pure helper (host-testable): host-order netmask for a CIDR prefix.
// prefix 26 -> 0xFFFFFFC0, prefix 29 -> 0xFFFFFFF8, prefix 0 -> 0.
uint32_t cfg_netmask_from_prefix(uint8_t prefix);

// Load config from NVS, seeding defaults for any missing key.
void cfg_load(aidlink_cfg_t *c);

// Persist config to NVS. Returns 0 on success, -1 on error.
int cfg_save(const aidlink_cfg_t *c);
