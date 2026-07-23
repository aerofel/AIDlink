// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "board.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdint.h>
#include "esp_mac.h"
#include "esp_log.h"

// The display drivers only exist on targets with the LCD peripheral. On the
// classic ESP32 the pointers resolve to NULL and the symbols are never
// referenced, so the linker never looks for them.
#if CONFIG_IDF_TARGET_ESP32S3
#include "paneldrv.h"
#define DISP(p, l)  .panel = &(p), .layout = &(l),
#else
#define DISP(p, l)  .panel = NULL, .layout = NULL,
#endif

// Fleet registry (see ESP32-BOARDS.md). Key = eFuse base MAC. When a new unit
// arrives, probe it with esptool and add its row here.
static const struct { uint8_t mac[6]; board_t b; } KNOWN[] = {
    { {0xe8,0x3d,0xc1,0xf7,0xa2,0x58},              // Board 2: S3 devkit N16R8
      { .name = "esp32s3-devkit",
        .panel = NULL, .layout = NULL,
        .led = LED_WS2812, .led_gpio = 48 } },

    { {0xd0,0xcf,0x13,0x32,0x2f,0x48},              // Board 3: LilyGO T-Display-S3
      { .name = "lilygo-tdisplay-s3",
        .disp_desc = "ST7789 320x170 display",
        DISP(panel_st7789, layout_tdisplay)
        // GPIO48 here is LCD data D7 — driving WS2812 pulses onto it would
        // corrupt the panel, so this board has no controllable LED.
        .led = LED_NONE, .led_gpio = -1 } },

    { {0x1c,0xdb,0xd4,0xbd,0x1f,0x1c},              // Board 4: LilyGO T3-S3 (LoRa)
      { .name = "lilygo-t3s3",
        .disp_desc = "SSD1306 128x64 OLED",
        DISP(panel_ssd1306, layout_oled)
        // plain green LED, not addressable; LoRa pins (5/3/6/7/8/34) untouched
        .led = LED_GPIO, .led_gpio = 37 } },
};

// Unknown unit: never drive display pins blind. Probing a WS2812 on GPIO48 is
// harmless on boards without one (statusled just logs a warning), and GPIO48 is
// the devkit convention, so that stays the generic guess.
static const board_t GENERIC = {
    .name = "generic", .panel = NULL, .layout = NULL,
    .led = LED_WS2812, .led_gpio = 48,
};

const board_t *board_get(void) {
    static const board_t *cached;
    if (cached) return cached;
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    cached = &GENERIC;
    for (unsigned i = 0; i < sizeof KNOWN / sizeof KNOWN[0]; i++)
        if (memcmp(mac, KNOWN[i].mac, 6) == 0) { cached = &KNOWN[i].b; break; }
    ESP_LOGI("board", "%02x:%02x:%02x:%02x:%02x:%02x -> %s (display=%s led=%s)",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], cached->name,
             cached->panel ? "yes" : "none",
             cached->led == LED_WS2812 ? "ws2812"
                                       : (cached->led == LED_GPIO ? "gpio" : "none"));
    return cached;
}
