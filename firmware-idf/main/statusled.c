// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Drives two WS2812 pixels on the onboard LED chain:
//   pixel 0 (the "big" status LED):
//       flashing orange = scanning for Wi-Fi
//       solid green     = Wi-Fi uplink connected
//       flashing red    = searching / not connected
//   pixel 1 (the "data" LED):
//       brief blue flash on every position frame sent to an EFB
#include "statusled.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3

#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "netcore.h"
#include "adbp.h"

// The one controllable LED: the onboard WS2812 on GPIO48. (The board's other
// small LEDs are hardwired UART/USB activity indicators, not GPIO-controllable.)
// It shows Wi-Fi state by color and blips blue on each position frame sent:
//   flashing orange = scanning · solid green = uplink connected ·
//   flashing red = searching · brief blue blip = position frame sent to an EFB
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
    uint32_t last_seq = adbp_push_seq();
    uint32_t blue_until = 0;
    for (;;) {
        uint32_t now = now_ms();
        uint32_t seq = adbp_push_seq();
        if (seq != last_seq) { last_seq = seq; blue_until = now + 90; }   // data sent -> blue blip

        if (now < blue_until) {
            px(0, 0, LVL);             // blue blip (overrides status briefly)
        } else {
            bool blink = (now / 300) % 2;
            uint8_t ip[4];
            if (netcore_scanning())      px(blink ? LVL : 0, blink ? LVL / 2 : 0, 0);  // orange flash
            else if (netcore_sta_up(ip)) px(0, LVL, 0);                                 // solid green
            else                         px(blink ? LVL : 0, 0, 0);                     // red flash
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

void statusled_start(void) {
    led_strip_config_t sc = { .strip_gpio_num = LED_GPIO, .max_leds = LED_COUNT };
    led_strip_rmt_config_t rc = { .resolution_hz = 10 * 1000 * 1000 };
    if (led_strip_new_rmt_device(&sc, &rc, &s_strip) != ESP_OK) {
        ESP_LOGW(TAG, "no RGB LED on GPIO%d", LED_GPIO);
        return;
    }
    led_strip_clear(s_strip);
    xTaskCreate(led_task, "statusled", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "status LED on GPIO%d (orange=scan, green=wifi, red=searching, blue blip=data)", LED_GPIO);
}

#else   // no onboard RGB LED (e.g. classic ESP32)
void statusled_start(void) {}
#endif
