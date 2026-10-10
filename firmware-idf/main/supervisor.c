// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "supervisor.h"
#include <stdbool.h>
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log.h"
#include "usb_ncm.h"

static const char *NAMES[SUP_N] = { "adbp", "poller", "dnsfwd", "display", "gps" };

static volatile uint32_t s_last[SUP_N];
static uint32_t s_limit[SUP_N];          // 0 = not watched

// Survives a software restart, not a power cycle (checked against the magic).
// The restart reason is parked here and logged after the reboot: the
// supervisor must not take ANY lock before restarting. A hung task may be the
// one holding the log or flash-log mutex (it logs from inside its loop), and
// a logln() here would then block the supervisor forever — seen on the bench.
#define SUP_MAGIC 0x53555056u
static RTC_NOINIT_ATTR uint32_t s_rtc_magic, s_rtc_restarts, s_rtc_why, s_rtc_silent_s;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void sup_watch(sup_id_t id, uint32_t limit_ms) {
    s_last[id] = now_ms();
    s_limit[id] = limit_ms;
}

void sup_beat(sup_id_t id) { s_last[id] = now_ms(); }

static void supervisor_task(void *arg) {
    bool cleared = false;
    // Watch the watcher: if this task ever blocks — including inside
    // esp_restart()'s shutdown handlers — the task WDT panics and reboots.
    esp_task_wdt_add(NULL);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_task_wdt_reset();
        uint32_t now = now_ms();
        if (!cleared && now >= SUP_HEALTHY_MS && s_rtc_restarts) {
            s_rtc_restarts = 0;          // no logln in this loop: see s_rtc_why
            cleared = true;
        }
        for (int i = 0; i < SUP_N; i++) {
            if (!s_limit[i]) continue;
            // Signed: another core may beat between our now_ms() and this read,
            // leaving s_last a millisecond AHEAD of now — unsigned, that wrapped
            // to ~4e9 ms "silent" and restarted a healthy unit.
            int32_t silent_ms = (int32_t)(now - s_last[i]);
            if (silent_ms < (int32_t)s_limit[i]) continue;
            uint32_t silent = (uint32_t)silent_ms;
            if (s_rtc_restarts >= SUP_MAX_RESTARTS) {
                // Budget spent: report and stay up. Console only — logln()
                // could block on the very lock the stalled task holds.
                s_last[i] = now;
                ESP_LOGW("sup", "%s silent %lus - restart budget spent, staying up",
                         NAMES[i], (unsigned long)(silent / 1000));
                continue;
            }
            s_rtc_why = (uint32_t)i + 1;
            s_rtc_silent_s = silent / 1000;
            s_rtc_restarts++;
            usb_ncm_restart();                // no app locks: reason is logged at next boot
        }
    }
}

void supervisor_start(void) {
    esp_reset_reason_t why = esp_reset_reason();
    if (s_rtc_magic != SUP_MAGIC || why == ESP_RST_POWERON || why == ESP_RST_BROWNOUT) {
        s_rtc_restarts = 0;
        s_rtc_why = 0;
    }
    s_rtc_magic = SUP_MAGIC;
    if (s_rtc_why && s_rtc_why <= SUP_N && why == ESP_RST_SW)
        logln("[sup] restarted: %s was silent %lus (supervisor restart #%lu)",
              NAMES[s_rtc_why - 1], (unsigned long)s_rtc_silent_s, (unsigned long)s_rtc_restarts);
    s_rtc_why = 0;
    // Above every application task (adbp/gps/dnsfwd/httpd 5, poller 4) so a
    // task spinning at its own priority cannot starve the check.
    xTaskCreate(supervisor_task, "sup", 3072, NULL, 10, NULL);
}
