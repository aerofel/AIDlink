// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Onboard status LED for boards without a display, in two flavours selected by
// board_get()->led:
//
//   LED_WS2812 (S3 devkit, GPIO48) — full colour:
//     slow red blink    = no Wi-Fi uplink
//     fast orange blink = scanning
//     steady orange     = connected, no internet
//     steady yellow     = connected + internet, weak signal
//     steady green      = connected + internet, strong signal
//     magenta blip      = a location was received from the feed (any state)
//
//   LED_GPIO (T3-S3, GPIO37) — one colour, so the same state machine is
//   re-expressed as a BLINK PATTERN, since brightness is all we have:
//     slow blink (0.8 Hz)   = no Wi-Fi uplink
//     fast blink (3 Hz)     = scanning
//     double-blink / second = connected, no internet
//     heartbeat (brief off) = connected + internet
//     solid 250 ms          = location received (overrides, like the blip)
#include "statusled.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3

#include "led_strip.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "netcore.h"
#include "pos.h"
#include "board.h"

#define LVL 40   // WS2812 brightness 0..255

static const char *TAG = "led";
static led_strip_handle_t s_strip;
static int s_gpio = -1;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void px(uint8_t r, uint8_t g, uint8_t b) {
    if (s_strip) { led_strip_set_pixel(s_strip, 0, r, g, b); led_strip_refresh(s_strip); }
}

static void ws2812_task(void *arg) {
    px(20, 20, 20);                    // boot: dim white
    vTaskDelay(pdMS_TO_TICKS(500));
    uint32_t last_seq = pos_fix_seq();
    uint32_t magenta_until = 0;
    for (;;) {
        uint32_t now = now_ms();
        uint32_t seq = pos_fix_seq();
        if (seq != last_seq) { last_seq = seq; magenta_until = now + 250; }

        if (now < magenta_until) {
            px(LVL, 0, LVL);                                       // magenta blip
        } else if (netcore_scanning()) {
            bool b = (now / 150) % 2;                              // fast, ~3 Hz
            px(b ? LVL : 0, b ? LVL / 2 : 0, 0);                   // orange flash
        } else if (netcore_sta_up(NULL)) {
            // weak/strong with hysteresis (mirrors the display's Wi-Fi fan)
            static bool weak;
            int rssi = netcore_sta_rssi();
            if (rssi >= -67) weak = false; else if (rssi <= -73) weak = true;
            if (!netcore_inet_up()) px(LVL, LVL / 2, 0);           // orange: no internet
            else if (weak)          px(LVL, LVL, 0);               // yellow: weak + internet
            else                    px(0, LVL, 0);                 // green: strong + internet
        } else {
            bool b = (now / 600) % 2;                              // slow, ~0.8 Hz
            px(b ? LVL : 0, 0, 0);                                 // red flash
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

// Single-colour LED: the state lives in the rhythm rather than the hue.
static void gpio_task(void *arg) {
    uint32_t last_seq = pos_fix_seq();
    uint32_t solid_until = 0;
    for (;;) {
        uint32_t now = now_ms();
        uint32_t seq = pos_fix_seq();
        if (seq != last_seq) { last_seq = seq; solid_until = now + 250; }

        bool on;
        if (now < solid_until) {
            on = true;                                   // fix received
        } else if (netcore_scanning()) {
            on = (now / 150) % 2;                        // fast blink
        } else if (netcore_sta_up(NULL)) {
            uint32_t ph = now % 1000;
            on = netcore_inet_up() ? (ph > 80)           // heartbeat: brief dip
                                   : (ph < 80 || (ph > 200 && ph < 280)); // double
        } else {
            on = (now / 600) % 2;                        // slow blink
        }
        gpio_set_level(s_gpio, on);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void statusled_start(void) {
    const board_t *b = board_get();
    s_gpio = b->led_gpio;

    switch (b->led) {
    case LED_WS2812: {
        // On the T-Display-S3, GPIO48 is LCD data D7 — driving WS2812 pulses
        // onto it would corrupt the display bus, which is why that board is
        // registered as LED_NONE rather than relying on a probe failing.
        led_strip_config_t sc = { .strip_gpio_num = s_gpio, .max_leds = 1 };
        led_strip_rmt_config_t rc = { .resolution_hz = 10 * 1000 * 1000 };
        if (led_strip_new_rmt_device(&sc, &rc, &s_strip) != ESP_OK) {
            ESP_LOGW(TAG, "no RGB LED on GPIO%d", s_gpio);
            return;
        }
        led_strip_clear(s_strip);
        xTaskCreate(ws2812_task, "statusled", 3072, NULL, 3, NULL);
        ESP_LOGI(TAG, "WS2812 status LED on GPIO%d (red=no wifi, orange=scan/no-inet, "
                      "yellow=weak+inet, green=strong+inet, magenta blip=fix)", s_gpio);
        break;
    }
    case LED_GPIO: {
        gpio_config_t gc = {
            .pin_bit_mask = 1ULL << s_gpio,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&gc);
        gpio_set_level(s_gpio, 0);
        xTaskCreate(gpio_task, "statusled", 2560, NULL, 3, NULL);
        ESP_LOGI(TAG, "GPIO status LED on GPIO%d (slow=no wifi, fast=scan, "
                      "double=no inet, heartbeat=online, solid=fix)", s_gpio);
        break;
    }
    default:
        ESP_LOGI(TAG, "no status LED on %s", b->name);
        break;
    }
}

#else   // no onboard LED support (e.g. classic ESP32)
void statusled_start(void) {}
#endif
