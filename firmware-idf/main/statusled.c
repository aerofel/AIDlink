// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Drives the onboard WS2812 RGB LED to show two states, alternating when both:
//   green   = Wi-Fi uplink (STA) connected
//   blue    = actively feeding position to an EFB (ADBP push)
//   green<->blue alternating = both at once
//   red     = neither (no uplink, not feeding)
//   dim white = boot
#include "statusled.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3

#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "netcore.h"
#include "adbp.h"

// ESP32-S3-DevKitC-1 onboard RGB LED. If your board's RGB LED is on a different
// pin (some use 38 or 47), change this one line.
#define LED_GPIO 48
#define LVL 40   // brightness 0..255; kept low so it isn't blinding

static const char *TAG = "led";
static led_strip_handle_t s_strip;

static void set(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

static void led_task(void *arg) {
    set(20, 20, 20);                       // boot: dim white
    vTaskDelay(pdMS_TO_TICKS(500));
    bool phase = false;                    // toggles for the green<->blue alternation
    for (;;) {
        uint8_t ip[4];
        bool wifi = netcore_sta_up(ip);
        bool feed = adbp_feeding();
        if (wifi && feed) {
            phase = !phase;
            set(phase ? 0 : 0, phase ? LVL : 0, phase ? 0 : LVL);  // alternate green / blue
        } else if (wifi) {
            set(0, LVL, 0);                // green — Wi-Fi connected
        } else if (feed) {
            set(0, 0, LVL);                // blue — feeding position (no uplink)
        } else {
            set(LVL, 0, 0);                // red — neither
        }
        vTaskDelay(pdMS_TO_TICKS(600));
    }
}

void statusled_start(void) {
    led_strip_config_t sc = { .strip_gpio_num = LED_GPIO, .max_leds = 1 };
    led_strip_rmt_config_t rc = { .resolution_hz = 10 * 1000 * 1000 };
    if (led_strip_new_rmt_device(&sc, &rc, &s_strip) != ESP_OK) {
        ESP_LOGW(TAG, "no RGB LED on GPIO%d", LED_GPIO);
        return;
    }
    led_strip_clear(s_strip);
    xTaskCreate(led_task, "statusled", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "status LED on GPIO%d (green=wifi, blue=feeding, alternate=both, red=neither)", LED_GPIO);
}

#else   // no onboard RGB LED (e.g. classic ESP32)
void statusled_start(void) {}
#endif
