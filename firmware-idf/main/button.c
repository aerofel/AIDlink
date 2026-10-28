// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "button.h"
#include "board.h"
#include "gps.h"
#include "touch.h"
#include "power.h"
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
#define PWR_RESTART_MS 1000   // power key: hold this long, release = restart
#define PWR_OFF_MS     3000   // ... this long = power off
#define OVL_HELD_MS    60000  // power overlays last while the key is held
#define PWR_SHOW_MS    700    // let the final overlay render before acting

static aidlink_cfg_t *CFG;
static btn_overlay_t  s_ovl;
static uint32_t       s_ovl_until;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

btn_overlay_t button_overlay(void) {
    if (s_ovl != BTN_OVL_NONE && (int32_t)(now_ms() - s_ovl_until) >= 0) s_ovl = BTN_OVL_NONE;
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

// Without a receiver the tap still answers, with the one thing every board
// has: the uplink. Otherwise a touch-only board would have a dead screen.
static void on_short_tap(void) {
    show(gps_available() ? BTN_OVL_DETAIL : BTN_OVL_LINK, OVL_DETAIL_MS);
}

// Power key. Ignored until it has been seen RELEASED once since boot, so the
// press that woke the board from power-off cannot count as a new hold.
static void power_key(int ppin, uint32_t t) {
    static int      stable = 0, last_raw = 0;   // assume held until proven not
    static uint32_t last_change, pressed_at;
    static bool     armed;
    static int      stage;                      // 0 none, 1 restart, 2 power off

    int raw = gpio_get_level(ppin);
    if (raw != last_raw) { last_raw = raw; last_change = t; }
    if (raw != stable && (t - last_change) >= DEBOUNCE_MS) {
        stable = raw;
        if (stable == 0) {                       // pressed
            pressed_at = t;
            stage = 0;
        } else if (!armed) {                     // first release since boot
            armed = true;
        } else if (stage == 2) {
            show(BTN_OVL_PWR_OFF, OVL_HELD_MS);
            vTaskDelay(pdMS_TO_TICKS(PWR_SHOW_MS));
            power_off("power key");
        } else if (stage == 1) {
            show(BTN_OVL_RESTARTING, OVL_HELD_MS);
            vTaskDelay(pdMS_TO_TICKS(PWR_SHOW_MS));
            power_restart("power key");
        }
    }
    if (armed && stable == 0) {
        uint32_t held = t - pressed_at;
        if (stage < 2 && held >= PWR_OFF_MS) {
            stage = 2;
            show(BTN_OVL_PWR_OFF_HOLD, OVL_HELD_MS);
        } else if (stage < 1 && held >= PWR_RESTART_MS) {
            stage = 1;
            show(BTN_OVL_PWR_RESTART, OVL_HELD_MS);
        }
    }
}

static void button_task(void *arg) {
    const int pin = board_get()->btn_gpio;
    const int ppin = board_get()->pwr_btn_gpio;
    int      stable = 1;         // active-low with a pull-up: 1 = released
    int      last_raw = 1;
    uint32_t last_change = 0;
    uint32_t pressed_at = 0;
    bool     long_fired = false;

    for (;;) {
        if (ppin >= 0) power_key(ppin, now_ms());

        // Key and touch panel are one input: either one held = pressed.
        int raw = (pin >= 0 && gpio_get_level(pin) == 0) || touch_pressed() ? 0 : 1;
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
    bool touch = touch_start();
    if (b->btn_gpio < 0 && b->pwr_btn_gpio < 0 && !touch) return;   // no input — stay inert

    CFG = cfg;
    if (b->pwr_btn_gpio >= 0) {
        gpio_config_t in = {
            .pin_bit_mask = 1ULL << b->pwr_btn_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&in);
        ESP_LOGI(TAG, "power key on GPIO%d (hold %d ms = restart, %d ms = power off)",
                 b->pwr_btn_gpio, PWR_RESTART_MS, PWR_OFF_MS);
    }
    if (b->btn_gpio >= 0) {
        gpio_config_t in = {
            .pin_bit_mask = 1ULL << b->btn_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&in);
        ESP_LOGI(TAG, "user key on GPIO%d (hold %d ms = swap source)", b->btn_gpio, LONG_MS);
    }
    // 5 KB: the long press runs cfg_save() (NVS) on this stack, the poll
    // includes an I2C transaction on touch boards, and power-off stops Wi-Fi
    // and TinyUSB from here.
    xTaskCreate(button_task, "button", 5120, NULL, 4, NULL);
}
