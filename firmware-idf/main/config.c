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

static void get_str(nvs_handle_t h, const char *k, char *buf, size_t bufsz) {
    size_t len = bufsz;
    nvs_get_str(h, k, buf, &len);   // leaves buf (seeded default) untouched on miss
}
static uint8_t get_u8(nvs_handle_t h, const char *k, uint8_t def) {
    uint8_t v; return nvs_get_u8(h, k, &v) == ESP_OK ? v : def;
}
static uint16_t get_u16(nvs_handle_t h, const char *k, uint16_t def) {
    uint16_t v; return nvs_get_u16(h, k, &v) == ESP_OK ? v : def;
}
static uint32_t get_u32(nvs_handle_t h, const char *k, uint32_t def) {
    uint32_t v; return nvs_get_u32(h, k, &v) == ESP_OK ? v : def;
}
static int32_t get_i32(nvs_handle_t h, const char *k, int32_t def) {
    int32_t v; return nvs_get_i32(h, k, &v) == ESP_OK ? v : def;
}
static bool get_bool(nvs_handle_t h, const char *k, bool def) {
    uint8_t v; return nvs_get_u8(h, k, &v) == ESP_OK ? (v != 0) : def;
}
static double get_dbl(nvs_handle_t h, const char *k, double def) {
    size_t len = sizeof(double); double v;
    return nvs_get_blob(h, k, &v, &len) == ESP_OK && len == sizeof(double) ? v : def;
}
static void get_ip(nvs_handle_t h, const char *k, uint8_t ip[4]) {
    size_t len = 4; uint8_t tmp[4];
    if (nvs_get_blob(h, k, tmp, &len) == ESP_OK && len == 4) memcpy(ip, tmp, 4);
}

void cfg_load(aidlink_cfg_t *c) {
    memset(c, 0, sizeof(*c));
    // --- seed defaults (used as-is when a key is absent from NVS) ---
    c->sta_dhcp = true;
    strcpy(c->sta_mask, "255.255.255.0");
    strcpy(c->ap_ssid, "AIDlink");
    strcpy(c->ap_pass, "88888888");
    c->ap_ip[0] = 172; c->ap_ip[1] = 20; c->ap_ip[2] = 1; c->ap_ip[3] = 1;
    c->ap_prefix = 26; c->ap_lease_min = 120; c->ap_dhcp_count = 60;
    c->ap_channel = 0; c->ap_max_clients = 8;
    c->usb_ip[0] = 172; c->usb_ip[1] = 20; c->usb_ip[2] = 2; c->usb_ip[3] = 1;
    c->usb_prefix = 29; c->napt_enable = true;
    strcpy(c->dev_name, "aidlink");
    c->adbp_port = 24000; c->ds_port = 51000;
    strcpy(c->api_ver, "3.1"); strcpy(c->ac_tail, "TEST01");
    c->src_type = 0;
    strcpy(c->vs_url, "http://192.168.4.2:8080/flight/info");
    c->poll_ms = 10000; c->stale_ms = 15000;
    c->auth_enable = true; strcpy(c->auth_user, "admin");

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored config; using defaults");
        return;
    }
    get_str(h, "sta_ssid", c->sta_ssid, sizeof c->sta_ssid);
    get_str(h, "sta_pass", c->sta_pass, sizeof c->sta_pass);
    c->sta_dhcp = get_bool(h, "sta_dhcp", c->sta_dhcp);
    get_str(h, "sta_ip", c->sta_ip, sizeof c->sta_ip);
    get_str(h, "sta_gw", c->sta_gw, sizeof c->sta_gw);
    get_str(h, "sta_mask", c->sta_mask, sizeof c->sta_mask);
    get_str(h, "sta_dns", c->sta_dns, sizeof c->sta_dns);
    get_str(h, "ap_ssid", c->ap_ssid, sizeof c->ap_ssid);
    get_str(h, "ap_pass", c->ap_pass, sizeof c->ap_pass);
    c->ap_hidden = get_bool(h, "ap_hidden", c->ap_hidden);
    get_ip(h, "ap_ip", c->ap_ip);
    c->ap_prefix = get_u8(h, "ap_prefix", c->ap_prefix);
    c->ap_lease_min = get_u16(h, "ap_lease_min", c->ap_lease_min);
    c->ap_dhcp_count = get_u8(h, "ap_dhcp_count", c->ap_dhcp_count);
    c->ap_channel = get_u8(h, "ap_channel", c->ap_channel);
    c->ap_max_clients = get_u8(h, "ap_max_clients", c->ap_max_clients);
    get_str(h, "ap_client_dns", c->ap_client_dns, sizeof c->ap_client_dns);
    get_ip(h, "usb_ip", c->usb_ip);
    c->usb_prefix = get_u8(h, "usb_prefix", c->usb_prefix);
    c->napt_enable = get_bool(h, "napt", c->napt_enable);
    get_str(h, "dev_name", c->dev_name, sizeof c->dev_name);
    c->adbp_port = get_u16(h, "adbp_port", c->adbp_port);
    c->ds_port = get_u16(h, "ds_port", c->ds_port);
    get_str(h, "api_ver", c->api_ver, sizeof c->api_ver);
    get_str(h, "ac_tail", c->ac_tail, sizeof c->ac_tail);
    get_str(h, "ac_type", c->ac_type, sizeof c->ac_type);
    c->src_type = get_i32(h, "src_type", c->src_type);
    get_str(h, "vs_url", c->vs_url, sizeof c->vs_url);
    c->poll_ms = get_u32(h, "poll_ms", c->poll_ms);
    c->stale_ms = get_u32(h, "stale_ms", c->stale_ms);
    c->sim_enable = get_bool(h, "sim_enable", c->sim_enable);
    c->sim_lat = get_dbl(h, "sim_lat", c->sim_lat);
    c->sim_lon = get_dbl(h, "sim_lon", c->sim_lon);
    c->sim_trk = get_dbl(h, "sim_trk", c->sim_trk);
    c->sim_gs = get_dbl(h, "sim_gs", c->sim_gs);
    c->sim_alt = get_dbl(h, "sim_alt", c->sim_alt);
    c->log_enable = get_bool(h, "log_enable", c->log_enable);
    c->auth_enable = get_bool(h, "auth_enable", c->auth_enable);
    get_str(h, "auth_user", c->auth_user, sizeof c->auth_user);
    get_str(h, "auth_hash", c->auth_hash, sizeof c->auth_hash);
    get_str(h, "auth_salt", c->auth_salt, sizeof c->auth_salt);
    nvs_close(h);
}

int cfg_save(const aidlink_cfg_t *c) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = ESP_OK;
    e |= nvs_set_str(h, "sta_ssid", c->sta_ssid);
    e |= nvs_set_str(h, "sta_pass", c->sta_pass);
    e |= nvs_set_u8(h, "sta_dhcp", c->sta_dhcp ? 1 : 0);
    e |= nvs_set_str(h, "sta_ip", c->sta_ip);
    e |= nvs_set_str(h, "sta_gw", c->sta_gw);
    e |= nvs_set_str(h, "sta_mask", c->sta_mask);
    e |= nvs_set_str(h, "sta_dns", c->sta_dns);
    e |= nvs_set_str(h, "ap_ssid", c->ap_ssid);
    e |= nvs_set_str(h, "ap_pass", c->ap_pass);
    e |= nvs_set_u8(h, "ap_hidden", c->ap_hidden ? 1 : 0);
    e |= nvs_set_blob(h, "ap_ip", c->ap_ip, 4);
    e |= nvs_set_u8(h, "ap_prefix", c->ap_prefix);
    e |= nvs_set_u16(h, "ap_lease_min", c->ap_lease_min);
    e |= nvs_set_u8(h, "ap_dhcp_count", c->ap_dhcp_count);
    e |= nvs_set_u8(h, "ap_channel", c->ap_channel);
    e |= nvs_set_u8(h, "ap_max_clients", c->ap_max_clients);
    e |= nvs_set_str(h, "ap_client_dns", c->ap_client_dns);
    e |= nvs_set_blob(h, "usb_ip", c->usb_ip, 4);
    e |= nvs_set_u8(h, "usb_prefix", c->usb_prefix);
    e |= nvs_set_u8(h, "napt", c->napt_enable ? 1 : 0);
    e |= nvs_set_str(h, "dev_name", c->dev_name);
    e |= nvs_set_u16(h, "adbp_port", c->adbp_port);
    e |= nvs_set_u16(h, "ds_port", c->ds_port);
    e |= nvs_set_str(h, "api_ver", c->api_ver);
    e |= nvs_set_str(h, "ac_tail", c->ac_tail);
    e |= nvs_set_str(h, "ac_type", c->ac_type);
    e |= nvs_set_i32(h, "src_type", c->src_type);
    e |= nvs_set_str(h, "vs_url", c->vs_url);
    e |= nvs_set_u32(h, "poll_ms", c->poll_ms);
    e |= nvs_set_u32(h, "stale_ms", c->stale_ms);
    e |= nvs_set_u8(h, "sim_enable", c->sim_enable ? 1 : 0);
    e |= nvs_set_blob(h, "sim_lat", &c->sim_lat, sizeof(double));
    e |= nvs_set_blob(h, "sim_lon", &c->sim_lon, sizeof(double));
    e |= nvs_set_blob(h, "sim_trk", &c->sim_trk, sizeof(double));
    e |= nvs_set_blob(h, "sim_gs", &c->sim_gs, sizeof(double));
    e |= nvs_set_blob(h, "sim_alt", &c->sim_alt, sizeof(double));
    e |= nvs_set_u8(h, "log_enable", c->log_enable ? 1 : 0);
    e |= nvs_set_u8(h, "auth_enable", c->auth_enable ? 1 : 0);
    e |= nvs_set_str(h, "auth_user", c->auth_user);
    e |= nvs_set_str(h, "auth_hash", c->auth_hash);
    e |= nvs_set_str(h, "auth_salt", c->auth_salt);
    if (e == ESP_OK) e |= nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}
