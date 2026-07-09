// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Drives the onboard WS2812 status LED (display-less boards):
//   slow red blink    = no Wi-Fi uplink
//   fast orange blink = scanning
//   steady orange     = connected, no internet
//   steady yellow     = connected + internet, weak signal
//   steady green      = connected + internet, strong signal
//   magenta blip      = a location was received from the feed (any state)
#include "statusled.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3

#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "netcore.h"
#include "pos.h"
#include "board.h"

// The one controllable LED: the onboard WS2812 on GPIO48. (The board's other
// small LEDs are hardwired UART/USB activity indicators, not GPIO-controllable.)
#define LED_GPIO   48
#define LED_COUNT  1
#define LVL 40   // brightness 0..255

static const char *TAG = "led";
static led_strip_handle_t s_strip;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void px(uint8_t r, uint8_t g, uint8_t b) {
    if (s_strip) { led_strip_set_pixel(s_strip, 0, r, g, b); led_strip_refresh(s_strip); }
}

static void led_task(void *arg) {
    px(20, 20, 20);                    // boot: dim white
    vTaskDelay(pdMS_TO_TICKS(500));
    uint32_t last_seq = pos_fix_seq();
    uint32_t magenta_until = 0;
    for (;;) {
        uint32_t now = now_ms();
        uint32_t seq = pos_fix_seq();
        if (seq != last_seq) { last_seq = seq; magenta_until = now + 250; }   // location received

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
            if (!netcore_inet_up()) px(LVL, LVL / 2, 0);           // steady orange: no internet
            else if (weak)          px(LVL, LVL, 0);               // steady yellow: weak + internet
            else                    px(0, LVL, 0);                 // steady green: strong + internet
        } else {
            bool b = (now / 600) % 2;                              // slow, ~0.8 Hz
            px(b ? LVL : 0, 0, 0);                                 // red flash
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

void statusled_start(void) {
    // On the T-Display-S3, GPIO48 is LCD data line D7 — driving WS2812 pulses
    // onto it would corrupt the display bus. Only boards that carry the LED.
    if (!board_get()->has_ws2812) {
        ESP_LOGI(TAG, "no WS2812 on %s — status LED disabled", board_get()->name);
        return;
    }
    led_strip_config_t sc = { .strip_gpio_num = LED_GPIO, .max_leds = LED_COUNT };
    led_strip_rmt_config_t rc = { .resolution_hz = 10 * 1000 * 1000 };
    if (led_strip_new_rmt_device(&sc, &rc, &s_strip) != ESP_OK) {
        ESP_LOGW(TAG, "no RGB LED on GPIO%d", LED_GPIO);
        return;
    }
    led_strip_clear(s_strip);
    xTaskCreate(led_task, "statusled", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "status LED on GPIO%d (red=no wifi, orange=scan/no-inet, yellow=weak+inet, green=strong+inet, magenta blip=fix)", LED_GPIO);
}

#else   // no onboard RGB LED (e.g. classic ESP32)
void statusled_start(void) {}
#endif
