// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "button.h"
#include "board.h"
#include "gps.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "button";

#define POLL_MS     20
#define DEBOUNCE_MS 30
#define LONG_MS     1000
#define OVL_SOURCE_MS 2000
#define OVL_DETAIL_MS 4000

static aidlink_cfg_t *CFG;
static btn_overlay_t  s_ovl;
static uint32_t       s_ovl_until;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

btn_overlay_t button_overlay(void) {
    if (s_ovl != BTN_OVL_NONE && now_ms() >= s_ovl_until) s_ovl = BTN_OVL_NONE;
    return s_ovl;
}

static void show(btn_overlay_t k, uint32_t ms) {
    s_ovl = k;
    s_ovl_until = now_ms() + ms;
}

// Both gestures are inert without a detected receiver: there is nothing to
// toggle to, and nothing to report.
static bool gps_available(void) {
    if (!gps_has_hw()) return false;
    gps_state_t g; gps_get(&g);
    return g.present;
}

static void on_long_press(void) {
    if (!gps_available()) return;
    CFG->gps_pref = (CFG->gps_pref == 1) ? 0 : 1;
    cfg_save(CFG);
    ESP_LOGI(TAG, "preferred position source -> %s",
             CFG->gps_pref == 1 ? "gps" : "feed");
    show(BTN_OVL_SOURCE, OVL_SOURCE_MS);
}

static void on_short_tap(void) {
    if (!gps_available()) return;
    show(BTN_OVL_DETAIL, OVL_DETAIL_MS);
}

static void button_task(void *arg) {
    const int pin = board_get()->btn_gpio;
    int      stable = 1;         // active-low with a pull-up: 1 = released
    int      last_raw = 1;
    uint32_t last_change = 0;
    uint32_t pressed_at = 0;
    bool     long_fired = false;

    for (;;) {
        int raw = gpio_get_level(pin);
        uint32_t t = now_ms();

        if (raw != last_raw) { last_raw = raw; last_change = t; }

        if (raw != stable && (t - last_change) >= DEBOUNCE_MS) {
            stable = raw;
            if (stable == 0) {                       // pressed
                pressed_at = t;
                long_fired = false;
            } else {                                 // released
                if (!long_fired && (t - pressed_at) < LONG_MS) on_short_tap();
            }
        }

        // Fire the long press while the key is still down, so the confirmation
        // appears under the finger rather than after letting go.
        if (stable == 0 && !long_fired && (t - pressed_at) >= LONG_MS) {
            long_fired = true;
            on_long_press();
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void button_start(aidlink_cfg_t *cfg) {
    const board_t *b = board_get();
    if (b->btn_gpio < 0) return;      // no user key on this model — stay inert

    CFG = cfg;
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << b->btn_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);
    ESP_LOGI(TAG, "user key on GPIO%d (hold %d ms = swap source)", b->btn_gpio, LONG_MS);
    xTaskCreate(button_task, "button", 3072, NULL, 4, NULL);
}
