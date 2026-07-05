# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AIDlink contributors
idf_component_register(SRCS "aidlink_main.c" "config.c" "config_util.c"
                            "dnsfwd.c" "dnsfwd_util.c" "netcore.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_wifi esp_netif esp_event lwip)
