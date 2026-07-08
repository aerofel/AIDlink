# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AIDlink contributors
set(srcs "aidlink_main.c" "config.c" "config_util.c"
         "dnsfwd.c" "dnsfwd_util.c" "netcore.c" "usb_ncm.c"
         "auth.c" "web.c" "web_util.c" "pos.c"
         "adbp.c" "adbp_frame.c"
         "poller.c" "poller_sources.c" "geo.c" "services.c" "log.c"
         "statusled.c" "board.c" "display.c" "airports.c"
         "tzdb.c" "tzdb_data.c" "derive.c" "font_arrow.c")
set(reqs nvs_flash esp_wifi esp_netif esp_event lwip
         esp_http_server esp_http_client esp-tls json
         mbedtls esp_timer esp_app_format mdns)

# TinyUSB (USB-NCM) only exists on targets with native USB-OTG.
if(CONFIG_SOC_USB_OTG_SUPPORTED)
    list(APPEND reqs esp_tinyusb)
endif()
# Onboard WS2812 RGB status LED + ST7789/LVGL flight display — S3 only.
if(CONFIG_IDF_TARGET_ESP32S3)
    list(APPEND reqs led_strip esp_lcd driver esp_lvgl_port lvgl)
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       REQUIRES ${reqs})
