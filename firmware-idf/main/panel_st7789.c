// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Board 3 — LilyGO T-Display-S3: ST7789 170x320 (used landscape 320x170)
// behind an Intel-8080 8-bit parallel bus. Factory pin map.
#include "paneldrv.h"

#if CONFIG_IDF_TARGET_ESP32S3

#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "log.h"

static const char *TAG = "panel.st7789";
#define DLOG(fmt, ...) do { ESP_LOGI(TAG, fmt, ##__VA_ARGS__); \
                            logln("[disp] " fmt, ##__VA_ARGS__); } while (0)

#define PIN_PWR_ON   15   // board/LCD power rail enable
#define PIN_BL       38   // backlight
#define PIN_RST      5
#define PIN_CS       6
#define PIN_DC       7
#define PIN_WR       8    // i80 write strobe (pclk)
#define PIN_RD       9    // read strobe — unused, must idle high
#define LCD_H        320  // landscape
#define LCD_V        170
#define LCD_GAP_Y    35   // 170-line panel sits at offset 35 in ST7789 RAM
#define LCD_PCLK_HZ  (8 * 1000 * 1000)

static bool st7789_init(lvgl_port_display_cfg_t *cfg)
{
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
    DLOG("gpio ok (pwr15=1 rd9=1 bl38=0), heap %u",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

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
        .dc_levels = { .dc_idle_level = 0, .dc_cmd_level = 0,
                       .dc_dummy_level = 0, .dc_data_level = 1 },
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

    *cfg = (lvgl_port_display_cfg_t){
        .io_handle = io,
        .panel_handle = panel,
        // Internal SRAM is scarce on this board (see LEARNING.md): a single
        // 20-line partial buffer (12.8 KB DMA) is plenty for a 1 Hz text UI.
        .buffer_size = LCD_H * 20,
        .double_buffer = false,
        .hres = LCD_H,
        .vres = LCD_V,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = { .swap_xy = true, .mirror_x = false, .mirror_y = true },
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    return true;
}

static void st7789_ready(void)
{
    gpio_set_level(PIN_BL, 1);
    DLOG("backlight on (%dx%d, i80 @ %d MHz)", LCD_H, LCD_V, LCD_PCLK_HZ / 1000000);
}

const panel_drv_t panel_st7789 = {
    .name = "st7789-320x170-i80",
    .init = st7789_init,
    .ready = st7789_ready,
};

#endif // CONFIG_IDF_TARGET_ESP32S3
