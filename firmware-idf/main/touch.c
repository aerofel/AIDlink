// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Hynitron CST816 capacitive touch (LilyGO T-Display-S3 Touch), polled.
//
// Polled rather than interrupt-driven: only "finger down / up" matters here,
// and the button task already ticks at 20 ms. That needs auto-sleep OFF —
// a sleeping CST816 does not answer I2C at all until it is touched or reset,
// which is also why init pulses RST first: after a soft reboot (/dfu, OTA,
// self-heal) the controller is still asleep from the previous run.
#include "touch.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3

#include "board.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log.h"

static const char *TAG = "touch";
#define TLOG(fmt, ...) do { ESP_LOGI(TAG, fmt, ##__VA_ARGS__); \
                            logln("[touch] " fmt, ##__VA_ARGS__); } while (0)

#define CST816_ADDR      0x15
#define REG_FINGER_NUM   0x02
#define REG_CHIP_ID      0xA7
#define REG_DIS_AUTOSLEEP 0xFE
#define I2C_TIMEOUT_MS   20

static i2c_master_dev_handle_t s_dev;
static int s_chip_id = -1;
static int s_rst_pin = -1;
static uint32_t s_fail_n;

static bool rd(uint8_t reg, uint8_t *buf, size_t n) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, I2C_TIMEOUT_MS) == ESP_OK;
}

bool touch_start(void) {
    const board_t *b = board_get();
    if (b->touch != TOUCH_CST816) return false;

    s_rst_pin = b->touch_rst;
    if (b->touch_rst >= 0) {
        gpio_config_t o = { .pin_bit_mask = 1ULL << b->touch_rst, .mode = GPIO_MODE_OUTPUT };
        gpio_config(&o);
        gpio_set_level(b->touch_rst, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(b->touch_rst, 1);
        vTaskDelay(pdMS_TO_TICKS(60));     // controller boot before first I2C
    }

    i2c_master_bus_config_t bc = {
        .i2c_port = -1,
        .sda_io_num = b->touch_sda,
        .scl_io_num = b->touch_scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) { TLOG("i2c bus init failed"); return false; }
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CST816_ADDR,
        .scl_speed_hz = 300000,
    };
    if (i2c_master_bus_add_device(bus, &dc, &s_dev) != ESP_OK) {
        i2c_del_master_bus(bus);
        TLOG("i2c add device failed");
        return false;
    }

    // Identify with a bounded register read, never i2c_master_probe(): that
    // call has no timeout and wedged the T3-S3 (LEARNING.md 2026-07-23).
    uint8_t id = 0;
    if (!rd(REG_CHIP_ID, &id, 1)) {
        TLOG("no CST816 at 0x%02x (sda%d/scl%d) — touch off", CST816_ADDR,
             b->touch_sda, b->touch_scl);
        i2c_master_bus_rm_device(s_dev);
        i2c_del_master_bus(bus);
        s_dev = NULL;
        return false;
    }
    uint8_t off[2] = { REG_DIS_AUTOSLEEP, 0x01 };
    esp_err_t e = i2c_master_transmit(s_dev, off, sizeof off, I2C_TIMEOUT_MS);
    TLOG("CST816 chip id 0x%02x, auto-sleep off: %s", id, esp_err_to_name(e));
    s_chip_id = id;
    return true;
}

// A CST816 that browned out or reset itself comes back in auto-sleep and then
// NACKs every read: touch would stay dead for the rest of the flight. After
// ~1 s of failed reads, pulse RST and turn auto-sleep off again (then retry
// that at most every ~5 s while it keeps failing).
static void recover(void) {
    if (s_rst_pin >= 0) {
        gpio_set_level(s_rst_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(s_rst_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    uint8_t off[2] = { REG_DIS_AUTOSLEEP, 0x01 };
    esp_err_t e = i2c_master_transmit(s_dev, off, sizeof off, I2C_TIMEOUT_MS);
    TLOG("controller not answering - reset: %s", esp_err_to_name(e));
}

bool touch_pressed(void) {
    if (!s_dev) return false;
    uint8_t n = 0;
    if (rd(REG_FINGER_NUM, &n, 1)) { s_fail_n = 0; return (n & 0x0F) > 0; }
    if (++s_fail_n == 50 || (s_fail_n > 50 && s_fail_n % 250 == 0)) recover();
    return false;
}

int touch_chip_id(void) { return s_chip_id; }

#else   // no touch hardware on targets without the LCD boards
bool touch_start(void) { return false; }
bool touch_pressed(void) { return false; }
int  touch_chip_id(void) { return -1; }
#endif
