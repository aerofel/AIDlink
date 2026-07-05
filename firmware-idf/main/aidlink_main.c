// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "config.h"
#include "netcore.h"
#include "usb_ncm.h"
#include "auth.h"
#include "web.h"

static const char *TAG = "aidlink";
static aidlink_cfg_t cfg;   // static: consulted by the web server for the device's lifetime

void app_main(void) {
    ESP_LOGI(TAG, "[aidlink-idf] boot %s", CONFIG_IDF_TARGET);

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    cfg_load(&cfg);

    // First-boot credential seed: default login admin / password (salted SHA-256).
    if (cfg.auth_hash[0] == 0) {
        auth_rand_hex(cfg.auth_salt, 8);
        auth_hash(cfg.auth_salt, "password", cfg.auth_hash);
        cfg.auth_enable = true;
        if (cfg.auth_user[0] == 0) strcpy(cfg.auth_user, "admin");
        cfg_save(&cfg);
        ESP_LOGI(TAG, "seeded default login admin/password");
    }
    auth_init();

    netcore_start(&cfg);
    usb_ncm_start(&cfg);   // USB-NCM cable networking (no-op without native USB)
    web_start(&cfg);
}
