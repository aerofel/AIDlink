// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Flight display for the LilyGO T-Display-S3: ST7789 170x320 (landscape 320x170)
// behind an Intel-8080 8-bit bus, driven by esp_lcd + LVGL (esp_lvgl_port).
// Layout:
//   F-ONEO                        SB800     tail (cyan) · flight number
//              NOU -> NRT                   route, large
//            2 431 NM TO GO                 great-circle distance to arrival
//   UTC+11                     14:23:05     solar-time zone + local time at position
//
// The UTC offset is the nautical/solar estimate round(lon/15) — a real IANA
// timezone lookup needs a shapefile database that has no place on this chip.
// The clock ticks from the last fix's UTC timestamp (Viasat provides it), so it
// stays right even when SNTP never ran.
#include "display.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "board.h"
#include "pos.h"
#include "geo.h"
#include "airports.h"
#include "tzdb.h"
#include "log.h"
#include "esp_heap_caps.h"

static const char *TAG = "disp";

// Bring-up evidence into the /log ring buffer: this board has no serial console
// (TinyUSB owns the only USB port), so the web /log endpoint is the only way to
// see how far display init got. Cheap enough to keep permanently.
#define DLOG(fmt, ...) do { ESP_LOGI(TAG, fmt, ##__VA_ARGS__); logln("[disp] " fmt, ##__VA_ARGS__); } while (0)

// --- LilyGO T-Display-S3 wiring (factory pin map) ---
#define PIN_PWR_ON   15   // board/LCD power rail enable
#define PIN_BL       38   // backlight
#define PIN_RST      5
#define PIN_CS       6
#define PIN_DC       7
#define PIN_WR       8    // i80 write strobe (pclk)
#define PIN_RD       9    // read strobe — unused, must idle high
#define LCD_H        320  // landscape
#define LCD_V        170
#define LCD_GAP_Y    35   // 170-line panel sits at offset 35 in the 240-line ST7789 RAM
#define LCD_PCLK_HZ  (8 * 1000 * 1000)

static const aidlink_cfg_t *CFG;
static lv_obj_t *s_tail, *s_flight, *s_route, *s_dist, *s_tz, *s_utc, *s_clock;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---- panel bring-up ----------------------------------------------------

static bool panel_init(esp_lcd_panel_io_handle_t *io_out, esp_lcd_panel_handle_t *panel_out) {
    // Power + control strap pins first: rail on, RD parked high.
    gpio_config_t gc = {
        .pin_bit_mask = (1ULL << PIN_PWR_ON) | (1ULL << PIN_RD) | (1ULL << PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&gc);
    gpio_set_level(PIN_PWR_ON, 1);
    gpio_set_level(PIN_RD, 1);
    // Backlight stays dark until the first frame is flushed: the ST7789's
    // graphics RAM survives resets, so an early backlight shows whatever the
    // previous firmware left there (the factory demo, memorably).
    gpio_set_level(PIN_BL, 0);
    DLOG("gpio ok (pwr15=1 rd9=1 bl38=0), heap %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    esp_lcd_i80_bus_handle_t bus = NULL;
    esp_lcd_i80_bus_config_t bc = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = PIN_DC,
        .wr_gpio_num = PIN_WR,
        .data_gpio_nums = { 39, 40, 41, 42, 45, 46, 47, 48 },
        .bus_width = 8,
        .max_transfer_bytes = LCD_H * 80 * sizeof(uint16_t),
        .psram_trans_align = 64,
        .sram_trans_align = 4,
    };
    esp_err_t e = esp_lcd_new_i80_bus(&bc, &bus);
    DLOG("i80 bus: %s", esp_err_to_name(e));
    if (e != ESP_OK) return false;

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i80_config_t ioc = {
        .cs_gpio_num = PIN_CS,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 20,
        .dc_levels = { .dc_idle_level = 0, .dc_cmd_level = 0, .dc_dummy_level = 0, .dc_data_level = 1 },
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    e = esp_lcd_new_panel_io_i80(bus, &ioc, &io);
    DLOG("i80 io: %s", esp_err_to_name(e));
    if (e != ESP_OK) return false;

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    e = esp_lcd_new_panel_st7789(io, &pc, &panel);
    DLOG("st7789 new: %s", esp_err_to_name(e));
    if (e != ESP_OK) return false;

    e = esp_lcd_panel_reset(panel);
    esp_err_t e2 = esp_lcd_panel_init(panel);
    DLOG("panel reset %s init %s", esp_err_to_name(e), esp_err_to_name(e2));
    esp_lcd_panel_invert_color(panel, true);      // this panel is IPS: inverted
    esp_lcd_panel_swap_xy(panel, true);           // landscape, USB-C to the right
    esp_lcd_panel_mirror(panel, false, true);
    esp_lcd_panel_set_gap(panel, 0, LCD_GAP_Y);
    e = esp_lcd_panel_disp_on_off(panel, true);
    DLOG("disp_on: %s", esp_err_to_name(e));

    *io_out = io; *panel_out = panel;
    return true;
}

// ---- screen construction (called under the LVGL lock) -------------------

static lv_obj_t *mklabel(lv_obj_t *parent, const lv_font_t *font, uint32_t hex,
                         lv_align_t align, int x, int y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(hex), 0);
    lv_obj_align(l, align, x, y);
    lv_label_set_text(l, "");
    return l;
}

static void build_ui(lv_display_t *disp) {
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_tail   = mklabel(scr, &lv_font_montserrat_20, 0x00E5FF, LV_ALIGN_TOP_LEFT,      8,   6);
    s_flight = mklabel(scr, &lv_font_montserrat_20, 0xFFFFFF, LV_ALIGN_TOP_RIGHT,    -8,   6);
    // 32pt fits two ICAO codes ("VTBS -> NWWW"); 40pt overflowed 320 px.
    s_route  = mklabel(scr, &lv_font_montserrat_32, 0xFFFFFF, LV_ALIGN_CENTER,        0, -18);
    s_dist   = mklabel(scr, &lv_font_montserrat_20, 0xFFB300, LV_ALIGN_CENTER,        0,  16);
    s_tz     = mklabel(scr, &lv_font_montserrat_16, 0x9E9E9E, LV_ALIGN_BOTTOM_LEFT,   8, -30);
    s_utc    = mklabel(scr, &lv_font_montserrat_20, 0x9E9E9E, LV_ALIGN_BOTTOM_LEFT,   8,  -6);
    s_clock  = mklabel(scr, &lv_font_montserrat_32, 0xFFFFFF, LV_ALIGN_BOTTOM_RIGHT, -8,  -4);
}

// ---- 1 Hz content refresh ------------------------------------------------

static void refresh(void) {
    pos_state_t p; pos_get(&p);
    char buf[48];

    lv_label_set_text(s_tail, p.tail[0] ? p.tail : CFG->ac_tail);
    lv_label_set_text(s_flight, p.flight[0] ? p.flight : CFG->ac_type);

    if (p.orig[0] || p.dest[0]) {
        // show ICAO codes; fall back to the code as received when unknown
        const char *o = airports_icao(p.orig); if (!o) o = p.orig[0] ? p.orig : "----";
        const char *d = airports_icao(p.dest); if (!d) d = p.dest[0] ? p.dest : "----";
        snprintf(buf, sizeof buf, "%s " LV_SYMBOL_RIGHT " %s", o, d);
        lv_label_set_text(s_route, buf);
    } else {
        lv_label_set_text(s_route, p.valid ? "- - -" : "NO POSITION");
    }
    // grey the route out while the fix is invalid/stale
    lv_obj_set_style_text_color(s_route, lv_color_hex(p.valid ? 0xFFFFFF : 0x616161), 0);

    double alat, alon;
    if (p.valid && p.dest[0] && airports_lookup(p.dest, &alat, &alon)) {
        int nm = (int)lround(geo_dist_nm(p.lat, p.lon, alat, alon));
        snprintf(buf, sizeof buf, "%d NM TO GO", nm);
        lv_label_set_text(s_dist, buf);
    } else if (p.valid && p.simulated) {
        lv_label_set_text(s_dist, "SIMULATED");
    } else {
        lv_label_set_text(s_dist, "");
    }

    // UTC: prefer the system clock (SNTP / HTTP-Date disciplined, see poller.c),
    // else tick the last fix's UTC timestamp forward.
    uint64_t utc = 0;
    time_t st = time(NULL);
    if (st > 1750000000) utc = (uint64_t)st * 1000ULL;
    else if (p.utc_ms) utc = p.utc_ms + (now_ms() - p.last_fix_ms);
    uint32_t utc_s = (uint32_t)(utc / 1000);

    // Local time at position from the embedded IANA-derived timezone grid
    // (handles DST and :30/:45 offsets); tzdb falls back to solar internally.
    int off_min = 0; bool have_off = false;
    if (p.valid || p.lat != 0 || p.lon != 0) {
        off_min = tzdb_offset_min(p.lat, p.lon, utc_s ? utc_s : 1767225600u);
        have_off = true;
    }

    if (!have_off) {
        lv_label_set_text(s_tz, "");
    } else if (off_min % 60) {
        snprintf(buf, sizeof buf, "UTC%+d:%02d", off_min / 60, abs(off_min % 60));
        lv_label_set_text(s_tz, buf);
    } else if (off_min == 0) {
        lv_label_set_text(s_tz, "UTC");
    } else {
        snprintf(buf, sizeof buf, "UTC%+d", off_min / 60);
        lv_label_set_text(s_tz, buf);
    }

    struct tm tm;
    if (utc) {
        time_t u = (time_t)(utc / 1000);
        gmtime_r(&u, &tm);
        snprintf(buf, sizeof buf, "%02d:%02d:%02dz", tm.tm_hour, tm.tm_min, tm.tm_sec);
        lv_label_set_text(s_utc, buf);
    } else {
        lv_label_set_text(s_utc, "--:--:--z");
    }
    if (utc && have_off) {
        time_t local = (time_t)(utc / 1000) + (time_t)off_min * 60;
        gmtime_r(&local, &tm);
        snprintf(buf, sizeof buf, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
        lv_label_set_text(s_clock, buf);
    } else {
        lv_label_set_text(s_clock, "--:--:--");
    }
}

static void display_task(void *arg) {
    bool first = true;
    for (;;) {
        if (lvgl_port_lock(100)) {
            refresh();
            lvgl_port_unlock();
            if (first) { first = false; DLOG("task alive, first refresh done"); }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void display_start(const aidlink_cfg_t *cfg) {
    // Force /log capture during bring-up so the evidence is web-readable on a
    // board with no serial console; restore the configured setting after.
    log_set_enable(true);   // diagnostic build: keep /log capture on
    DLOG("board=%s has_display=%d", board_get()->name, board_get()->has_display);
    if (!board_get()->has_display) return;
    CFG = cfg;

    esp_lcd_panel_io_handle_t io; esp_lcd_panel_handle_t panel;
    if (!panel_init(&io, &panel)) return;

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    esp_err_t pe = lvgl_port_init(&port_cfg);
    DLOG("lvgl_port_init: %s", esp_err_to_name(pe));
    if (pe != ESP_OK) return;

    const lvgl_port_display_cfg_t dc = {
        .io_handle = io,
        .panel_handle = panel,
        // Internal SRAM is scarce here (see LEARNING.md): a single 20-line
        // partial buffer (12.8 KB DMA) is plenty for a 1 Hz textual UI.
        .buffer_size = LCD_H * 20,
        .double_buffer = false,
        .hres = LCD_H,
        .vres = LCD_V,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = { .swap_xy = true, .mirror_x = false, .mirror_y = true },
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    lv_display_t *disp = lvgl_port_add_disp(&dc);
    DLOG("lvgl_port_add_disp: %s, heap %u", disp ? "ok" : "NULL",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (!disp) return;

    if (lvgl_port_lock(0)) {
        build_ui(disp);
        refresh();
        lvgl_port_unlock();
        DLOG("ui built");
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    gpio_set_level(PIN_BL, 1);

    xTaskCreate(display_task, "display", 3072, NULL, 3, NULL);
    DLOG("display up (320x170, i80 @ %d MHz)", LCD_PCLK_HZ / 1000000);
}

#else   // targets without the LCD peripheral (classic ESP32)
void display_start(const aidlink_cfg_t *cfg) { (void)cfg; }
#endif
