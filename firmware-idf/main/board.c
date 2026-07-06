// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "board.h"
#include <string.h>
#include <stdint.h>
#include "esp_mac.h"
#include "esp_log.h"

// Fleet registry (see ESP32-BOARDS.md). Key = eFuse base MAC. When a new unit
// arrives, probe it with esptool and add its row here.
static const struct { uint8_t mac[6]; board_t b; } KNOWN[] = {
    { {0xe8,0x3d,0xc1,0xf7,0xa2,0x58},              // Board 2: S3 devkit N16R8
      { .name = "esp32s3-devkit", .has_ws2812 = true,  .has_display = false } },
    { {0xd0,0xcf,0x13,0x32,0x2f,0x48},              // Board 3: LilyGO T-Display-S3
      { .name = "lilygo-tdisplay-s3", .has_ws2812 = false, .has_display = true } },
};

// Unknown unit: no display (never drive LCD pins blind); probing the WS2812 is
// harmless on boards without one (statusled just logs a warning).
static const board_t GENERIC = { .name = "generic", .has_ws2812 = true, .has_display = false };

const board_t *board_get(void) {
    static const board_t *cached;
    if (cached) return cached;
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    cached = &GENERIC;
    for (unsigned i = 0; i < sizeof KNOWN / sizeof KNOWN[0]; i++)
        if (memcmp(mac, KNOWN[i].mac, 6) == 0) { cached = &KNOWN[i].b; break; }
    ESP_LOGI("board", "%02x:%02x:%02x:%02x:%02x:%02x -> %s%s%s",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], cached->name,
             cached->has_display ? " +display" : "", cached->has_ws2812 ? " +ws2812" : "");
    return cached;
}
