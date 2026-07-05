// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// NVS-backed config load/save. Pure helpers live in config_util.c so they stay
// host-testable; this file holds the ESP-IDF-dependent persistence.
#include "config.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#define NS "aidlink"
static const char *TAG = "cfg";

static void get_str(nvs_handle_t h, const char *k, char *buf, size_t bufsz, const char *def) {
    size_t len = bufsz;
    if (nvs_get_str(h, k, buf, &len) != ESP_OK) {
        if (buf != def) { strncpy(buf, def, bufsz - 1); buf[bufsz - 1] = 0; }
    }
}
static uint8_t get_u8(nvs_handle_t h, const char *k, uint8_t def) {
    uint8_t v; return nvs_get_u8(h, k, &v) == ESP_OK ? v : def;
}
static uint16_t get_u16(nvs_handle_t h, const char *k, uint16_t def) {
    uint16_t v; return nvs_get_u16(h, k, &v) == ESP_OK ? v : def;
}
static void get_ip(nvs_handle_t h, const char *k, uint8_t ip[4]) {
    size_t len = 4; uint8_t tmp[4];
    if (nvs_get_blob(h, k, tmp, &len) == ESP_OK && len == 4) memcpy(ip, tmp, 4);
}

void cfg_load(aidlink_cfg_t *c) {
    memset(c, 0, sizeof(*c));
    // seed defaults (used as-is when a key is absent from NVS)
    strcpy(c->ap_ssid, "AIDlink");
    strcpy(c->ap_pass, "88888888");
    c->ap_ip[0] = 172; c->ap_ip[1] = 20; c->ap_ip[2] = 1; c->ap_ip[3] = 1;
    c->ap_prefix = 26; c->ap_lease_min = 120; c->ap_dhcp_count = 60;
    c->usb_ip[0] = 172; c->usb_ip[1] = 20; c->usb_ip[2] = 2; c->usb_ip[3] = 1;
    c->usb_prefix = 29; c->napt_enable = true;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored config; using defaults");
        return;
    }
    get_str(h, "sta_ssid", c->sta_ssid, sizeof c->sta_ssid, c->sta_ssid);
    get_str(h, "sta_pass", c->sta_pass, sizeof c->sta_pass, c->sta_pass);
    get_str(h, "ap_ssid", c->ap_ssid, sizeof c->ap_ssid, c->ap_ssid);
    get_str(h, "ap_pass", c->ap_pass, sizeof c->ap_pass, c->ap_pass);
    get_ip(h, "ap_ip", c->ap_ip);
    c->ap_prefix = get_u8(h, "ap_prefix", c->ap_prefix);
    c->ap_lease_min = get_u16(h, "ap_lease_min", c->ap_lease_min);
    c->ap_dhcp_count = get_u8(h, "ap_dhcp_count", c->ap_dhcp_count);
    get_str(h, "ap_client_dns", c->ap_client_dns, sizeof c->ap_client_dns, c->ap_client_dns);
    get_ip(h, "usb_ip", c->usb_ip);
    c->usb_prefix = get_u8(h, "usb_prefix", c->usb_prefix);
    c->napt_enable = get_u8(h, "napt", c->napt_enable ? 1 : 0) != 0;
    nvs_close(h);
}

int cfg_save(const aidlink_cfg_t *c) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = ESP_OK;
    e |= nvs_set_str(h, "sta_ssid", c->sta_ssid);
    e |= nvs_set_str(h, "sta_pass", c->sta_pass);
    e |= nvs_set_str(h, "ap_ssid", c->ap_ssid);
    e |= nvs_set_str(h, "ap_pass", c->ap_pass);
    e |= nvs_set_blob(h, "ap_ip", c->ap_ip, 4);
    e |= nvs_set_u8(h, "ap_prefix", c->ap_prefix);
    e |= nvs_set_u16(h, "ap_lease_min", c->ap_lease_min);
    e |= nvs_set_u8(h, "ap_dhcp_count", c->ap_dhcp_count);
    e |= nvs_set_str(h, "ap_client_dns", c->ap_client_dns);
    e |= nvs_set_blob(h, "usb_ip", c->usb_ip, 4);
    e |= nvs_set_u8(h, "usb_prefix", c->usb_prefix);
    e |= nvs_set_u8(h, "napt", c->napt_enable ? 1 : 0);
    if (e == ESP_OK) e |= nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}
