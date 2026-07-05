# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AIDlink contributors
set(srcs "aidlink_main.c" "config.c" "config_util.c"
         "dnsfwd.c" "dnsfwd_util.c" "netcore.c" "usb_ncm.c"
         "auth.c" "web.c" "web_util.c" "pos.c")
set(reqs nvs_flash esp_wifi esp_netif esp_event lwip
         esp_http_server mbedtls esp_timer esp_app_format)

# TinyUSB (USB-NCM) only exists on targets with native USB-OTG.
if(CONFIG_SOC_USB_OTG_SUPPORTED)
    list(APPEND reqs esp_tinyusb)
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       REQUIRES ${reqs})
