// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include <stdio.h>
#include "esp_system.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "config.h"
#include "netcore.h"

static const char *TAG = "aidlink";

void app_main(void) {
    ESP_LOGI(TAG, "[aidlink-idf] boot %s", CONFIG_IDF_TARGET);

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    aidlink_cfg_t cfg;
    cfg_load(&cfg);
    netcore_start(&cfg);
}
