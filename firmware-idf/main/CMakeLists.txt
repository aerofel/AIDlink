# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AIDlink contributors
set(srcs "aidlink_main.c" "config.c" "config_util.c"
         "dnsfwd.c" "dnsfwd_util.c" "netcore.c" "usb_ncm.c"
         "auth.c" "web.c" "web_util.c" "pos.c"
         "adbp.c" "adbp_frame.c"
         "poller.c" "poller_sources.c" "geo.c" "services.c" "log.c"
         "statusled.c" "board.c" "display.c" "airports.c"
         "tzdb.c" "tzdb_data.c" "derive.c" "eta.c" "font_arrow.c" "font_cloud.c" "font_tod.c" "font_eta.c"
         "perfdb.c" "perfdb_data.c" "eta_profile.c"
         # board-independent view model + the per-board panel/layout drivers
         "flightview.c"
         "panel_st7789.c" "layout_tdisplay.c"
         "panel_ssd1306.c" "layout_oled.c"
         # 1bpp fonts for the mono OLED (tools/gen_oled_fonts.sh)
         "font_aid8.c" "font_aid12.c" "font_aid16.c")
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

# Auto-incrementing firmware build number: bump ../buildnum.txt (committed,
# shared by both targets) and regenerate buildnum.h on EVERY build. Shown on
# the display splash row and the portal hero/status strip.
idf_build_get_property(python PYTHON)
set(BUILDNUM_H ${CMAKE_CURRENT_BINARY_DIR}/buildnum.h)
add_custom_target(buildnum ALL
    COMMAND ${python} ${COMPONENT_DIR}/../../tools/bump_buildnum.py
            ${COMPONENT_DIR}/../buildnum.txt ${BUILDNUM_H}
    BYPRODUCTS ${BUILDNUM_H})
add_dependencies(${COMPONENT_LIB} buildnum)
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
