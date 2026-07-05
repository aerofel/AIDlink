// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include <stdio.h>
#include "esp_system.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "aidlink";

void app_main(void) {
    ESP_LOGI(TAG, "[aidlink-idf] boot %s", CONFIG_IDF_TARGET);
}
