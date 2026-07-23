// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Board 4 — LilyGO T3-S3: 0.96" SSD1306 128x64 monochrome OLED on I2C,
// probed live at 0x3C on SDA 18 / SCL 17 (see ESP32-BOARDS.md).
//
// Two hard-won details, both verified on the bench (LEARNING.md 2026-07-23):
//
//  * double_buffer MUST be true. esp_lvgl_port's monochrome path converts into
//    a FULL-screen, page-mapped buffer indexed from (0,0), then hands that
//    pointer to esp_lcd_panel_draw_bitmap along with LVGL's DIRTY rectangle
//    (esp_lvgl_port_disp.c:754). With one buffer the dirty rect is partial and
//    the two disagree — glyphs come out sheared and fragmented.
//
//  * Do NOT call esp_lcd_panel_invert_color(). The port's transform already
//    writes a SET bit (= lit pixel) for BLACK source pixels, so LVGL's default
//    light theme lands on the panel as bright text on black, which is what the
//    colour board does too. Inverting here would double-negate it.
#include "paneldrv.h"

#if CONFIG_IDF_TARGET_ESP32S3

#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "log.h"

static const char *TAG = "panel.ssd1306";
#define DLOG(fmt, ...) do { ESP_LOGI(TAG, fmt, ##__VA_ARGS__); \
                            logln("[disp] " fmt, ##__VA_ARGS__); } while (0)

#define PIN_SDA    18
#define PIN_SCL    17
#define OLED_ADDR  0x3C
#define OLED_W     128
#define OLED_H     64

static bool ssd1306_init(lvgl_port_display_cfg_t *cfg)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = -1,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t e = i2c_new_master_bus(&bc, &bus);
    DLOG("i2c bus (sda%d/scl%d): %s", PIN_SDA, PIN_SCL, esp_err_to_name(e));
    if (e != ESP_OK) return false;

    // NOTE: deliberately no i2c_master_probe() here. On this board it spins
    // forever in i2c_ll_is_bus_busy() (i2c_master.c:543, no timeout) and would
    // hang the display task before it ever started. A missing panel instead
    // surfaces as an error from esp_lcd_panel_init below, which is recoverable.
    esp_lcd_panel_io_i2c_config_t ioc = {
        .dev_addr = OLED_ADDR,
        .scl_speed_hz = 400000,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_bit_offset = 6,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    e = esp_lcd_new_panel_io_i2c(bus, &ioc, &io);
    DLOG("panel io (0x%02X): %s", OLED_ADDR, esp_err_to_name(e));
    if (e != ESP_OK) return false;

    esp_lcd_panel_ssd1306_config_t vendor = { .height = OLED_H };
    esp_lcd_panel_dev_config_t pc = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,          // no RST strap on this board
        .vendor_config = &vendor,
    };
    esp_lcd_panel_handle_t panel = NULL;
    e = esp_lcd_new_panel_ssd1306(io, &pc, &panel);
    DLOG("ssd1306 new: %s", esp_err_to_name(e));
    if (e != ESP_OK) return false;

    e = esp_lcd_panel_reset(panel);
    esp_err_t e2 = esp_lcd_panel_init(panel);
    DLOG("panel reset %s init %s", esp_err_to_name(e), esp_err_to_name(e2));
    if (e2 != ESP_OK) return false;    // nothing acked -> stay headless
    e = esp_lcd_panel_disp_on_off(panel, true);
    DLOG("disp_on: %s", esp_err_to_name(e));

    *cfg = (lvgl_port_display_cfg_t){
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = OLED_W * OLED_H,   // monochrome demands a full buffer
        .double_buffer = true,            // REQUIRED — see header comment
        .hres = OLED_W,
        .vres = OLED_H,
        .monochrome = true,
        .color_format = LV_COLOR_FORMAT_I1,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags = { .buff_dma = false, .swap_bytes = false, .sw_rotate = false },
    };
    return true;
}

static void ssd1306_ready(void)
{
    DLOG("oled up (%dx%d mono, i2c @ 400 kHz)", OLED_W, OLED_H);
}

const panel_drv_t panel_ssd1306 = {
    .name = "ssd1306-128x64-i2c",
    .init = ssd1306_init,
    .ready = ssd1306_ready,
};

#endif // CONFIG_IDF_TARGET_ESP32S3
