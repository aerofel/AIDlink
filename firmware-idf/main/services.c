// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "services.h"
#include <string.h>
#include <ctype.h>
#include "mdns.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"

static const char *TAG = "mdns";

// Sanitize dev_name -> a valid mDNS host label: lowercase, [a-z0-9-], collapse
// runs of '-' (mirrors the v9 behavior).
static void sanitize_host(const char *in, char *out, size_t cap) {
    size_t o = 0; char prev = 0;
    for (const char *p = in; *p && o + 1 < cap; p++) {
        char c = tolower((unsigned char)*p);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) c = '-';
        if (c == '-' && prev == '-') continue;     // collapse "--"
        out[o++] = c; prev = c;
    }
    while (o > 0 && out[o - 1] == '-') o--;         // trim trailing '-'
    if (o == 0) { strncpy(out, "aidlink", cap - 1); out[cap - 1] = 0; return; }
    out[o] = 0;
}

void services_start(const aidlink_cfg_t *cfg) {
    char host[32];
    sanitize_host(cfg->dev_name[0] ? cfg->dev_name : "aidlink", host, sizeof host);
    if (mdns_init() != ESP_OK) { ESP_LOGE(TAG, "mdns_init failed"); return; }
    mdns_hostname_set(host);
    mdns_instance_name_set("AIDlink");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    mdns_service_add(NULL, "_aidlink-adbp", "_tcp", cfg->adbp_port, NULL, 0);
    ESP_LOGI(TAG, "[mDNS] http://%s.local/  (adbp on %u)", host, cfg->adbp_port);

    // SNTP: disciplines the clock whenever the uplink actually reaches the
    // internet; retries quietly forever otherwise. The poller's HTTP-Date
    // fallback covers the walled-garden case (see poller.c).
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&sc) == ESP_OK) ESP_LOGI(TAG, "SNTP started (pool.ntp.org)");
}
