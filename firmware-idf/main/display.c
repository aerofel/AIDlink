// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Onboard flight display — orchestration only.
//
// This file owns the display task, the LVGL lock and the health heartbeat.
// It does not know what panel it is driving or what the screen looks like:
// board_get() supplies a panel_drv_t (hardware) and a layout_drv_t (pixels),
// and flightview_build() supplies the numbers. See paneldrv.h.
//
// Boards without a display (classic ESP32, the S3 devkit, any unknown unit)
// fall through harmlessly: board->panel is NULL and display_start() returns.
#include "display.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "board.h"
#include "paneldrv.h"
#include "flightview.h"
#include "log.h"

static const char *TAG = "disp";

// Bring-up evidence into the /log ring buffer: these boards have no serial
// console (TinyUSB owns the only USB port), so the web /log endpoint is the
// only way to see how far display init got. Cheap enough to keep permanently.
#define DLOG(fmt, ...) do { ESP_LOGI(TAG, fmt, ##__VA_ARGS__); \
                            logln("[disp] " fmt, ##__VA_ARGS__); } while (0)

static const layout_drv_t *LAYOUT;

static void display_task(void *arg)
{
    bool first = true;
    // 100 ms tick: indicators blink every tick, the heavier content refresh
    // keeps its 500 ms cadence.
    //
    // Diagnostics (2026-07-09 freeze hunt): LV_USE_ASSERT_MALLOC's default
    // handler is while(1) — a failed LVGL pool alloc silently hangs the render
    // task while it holds the lock, freezing the screen with the device alive.
    // The heartbeat below surfaces pool usage in /log; the stuck-lock warning
    // distinguishes "render task hung" from "our refresh crashed".
    int lockfail = 0;
    for (int tick = 0;; tick++) {
        if (lvgl_port_lock(100)) {
            lockfail = 0;
            if (tick % 5 == 0) {
                flightview_t v;
                flightview_build(&v);
                LAYOUT->render(&v);
            }
            fv_status_t s;
            flightview_status(&s);
            LAYOUT->status(&s);
            if (tick % 600 == 0) {   // ~60 s heartbeat with LVGL pool stats
                lv_mem_monitor_t m;
                lv_mem_monitor(&m);
                DLOG("hb tick=%d lv used=%d%% frag=%d%% free=%u biggest=%u",
                     tick, m.used_pct, m.frag_pct,
                     (unsigned)m.free_size, (unsigned)m.free_biggest_size);
            }
            lvgl_port_unlock();
            if (first) { first = false; DLOG("task alive, first refresh done"); }
        } else if (++lockfail == 20) {
            DLOG("LVGL lock stuck 2+ s — render task hung (pool exhausted?)");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void display_start(const aidlink_cfg_t *cfg)
{
    // Force /log capture during bring-up so the evidence is web-readable on a
    // board with no serial console.
    log_set_enable(true);

    const board_t *b = board_get();
    DLOG("board=%s panel=%s layout=%s", b->name,
         b->panel ? b->panel->name : "none",
         b->layout ? b->layout->name : "none");
    if (!b->panel || !b->layout) return;
    LAYOUT = b->layout;

    flightview_init(cfg);

    lvgl_port_display_cfg_t dc;
    if (!b->panel->init(&dc)) { DLOG("panel init failed — staying headless"); return; }

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    esp_err_t pe = lvgl_port_init(&port_cfg);
    DLOG("lvgl_port_init: %s", esp_err_to_name(pe));
    if (pe != ESP_OK) return;

    lv_display_t *disp = lvgl_port_add_disp(&dc);
    DLOG("lvgl_port_add_disp: %s, heap %u", disp ? "ok" : "NULL",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (!disp) return;

    if (lvgl_port_lock(0)) {
        LAYOUT->build(disp);
        flightview_t v;
        flightview_build(&v);
        LAYOUT->render(&v);
        lvgl_port_unlock();
        DLOG("ui built (%s)", LAYOUT->name);
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    if (b->panel->ready) b->panel->ready();

    // 4 KB stack: the render path walks the theoretical-profile estimator
    // (deep double math + gmtime/snprintf); 3 KB panicked once the first valid
    // fix arrived — the profile table itself lives in etap_state_t, NOT here.
    xTaskCreate(display_task, "display", 4096, NULL, 3, NULL);
}

#else   // targets without the LCD peripheral (classic ESP32)
void display_start(const aidlink_cfg_t *cfg) { (void)cfg; }
#endif
