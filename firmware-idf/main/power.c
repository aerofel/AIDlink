// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "power.h"
#include "board.h"
#include "display.h"
#include "log.h"
#include "usb_ncm.h"
#include "sdkconfig.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if SOC_RTCIO_INPUT_OUTPUT_SUPPORTED
#include "driver/rtc_io.h"
#endif

void power_restart(const char *why)
{
    logln("[pwr] restart (%s)", why);
    vTaskDelay(pdMS_TO_TICKS(150));      // let the line reach the flash log
    usb_ncm_stop();                      // detach first, as /dfu does
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}

void power_off(const char *why)
{
    logln("[pwr] power off (%s)", why);
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_wifi_stop();
    usb_ncm_stop();
    display_off();

#if CONFIG_IDF_TARGET_ESP32S3
    // Wake on either side button: the power key itself, and the user key so
    // the board also comes back from the button people press first.
    const board_t *b = board_get();
    uint64_t mask = 0;
    int pins[2] = { b->pwr_btn_gpio, b->btn_gpio };
    for (int i = 0; i < 2; i++) {
        if (pins[i] < 0 || !rtc_gpio_is_valid_gpio(pins[i])) continue;
        mask |= 1ULL << pins[i];
        // Both keys have external pull-ups on the T-Display-S3; the internal
        // ones are belt and braces (RTC periph stays powered for them).
        rtc_gpio_pullup_en(pins[i]);
        rtc_gpio_pulldown_dis(pins[i]);
    }
    if (mask) {
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
        esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    }
    esp_deep_sleep_start();
#else
    esp_restart();                       // no board without a power key gets here
#endif
}
