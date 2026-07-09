// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// USB-NCM cable networking (ESP32-S3 native USB). No-op on targets without
// native USB (guarded internally by CONFIG_SOC_USB_OTG_SUPPORTED).
#pragma once
#include "config.h"

// Bring up the USB-NCM network device: a private DHCP+NAPT subnet routed to the
// Wi-Fi STA uplink, so a USB host (Mac) plugged into the native USB gets internet
// through the S3 — coexisting with the SoftAP.
void usb_ncm_start(const aidlink_cfg_t *c);

// Cleanly detach from the USB host (TinyUSB uninstall) — call before rebooting
// into the ROM downloader (/dfu) so the port re-enumerates reliably.
void usb_ncm_stop(void);

// If a USB-cable host is currently connected, fill mac_str (>=18) and ip_str
// (>=16) and return true; else false. Always false on non-USB targets.
#include <stdbool.h>
bool usb_ncm_client(char *mac_str, char *ip_str);
