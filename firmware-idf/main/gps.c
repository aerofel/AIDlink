// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "gps.h"
#include <string.h>
#include "board.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "gps";

#define GPS_UART      UART_NUM_1
#define GPS_BAUD      9600     // u-blox default; the bench unit runs stock NMEA
#define RX_BUF        2048
#define LINE_MAX      128
#define PRESENT_HOLD  60000    // ms of silence before we stop claiming a receiver
#define M_TO_FT       3.280839895

static gps_state_t      s_gps;
static SemaphoreHandle_t s_mux;

// PPS is edge-counted rather than timed against a reference: we only report that
// the receiver has time lock, we do not discipline the clock from it.
static volatile uint32_t s_pps_edges;
static volatile int64_t  s_pps_last_us, s_pps_interval_us;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void IRAM_ATTR pps_isr(void *arg) {
    int64_t now = esp_timer_get_time();
    if (s_pps_last_us) s_pps_interval_us = now - s_pps_last_us;
    s_pps_last_us = now;
    s_pps_edges++;
}

bool gps_has_hw(void) { return board_get()->gps_rx >= 0; }

void gps_get(gps_state_t *out) {
    if (!s_mux) { memset(out, 0, sizeof *out); return; }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_gps;
    xSemaphoreGive(s_mux);
}

static void publish(const nmea_state_t *n, bool fix_ok) {
    uint32_t t = now_ms();
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_gps.present    = true;
    s_gps.last_rx_ms = t;
    s_gps.fix        = n->fix;
    s_gps.sats_used  = n->sats_used;
    s_gps.sats_view  = n->sats_view;
    s_gps.sats_gps   = n->sats_gps;
    s_gps.sats_glo   = n->sats_glo;
    s_gps.sats_gal   = n->sats_gal;
    s_gps.sats_bds   = n->sats_bds;
    s_gps.sats_qzss  = n->sats_qzss;
    s_gps.hdop       = n->hdop;
    s_gps.gs_kt      = n->gs_kt;
    s_gps.track_deg  = n->track_deg;
    s_gps.utc_ms     = n->utc_ms;
    if (fix_ok) {
        s_gps.lat = n->lat;
        s_gps.lon = n->lon;
        s_gps.alt_ft = n->alt_m * M_TO_FT;
        s_gps.last_fix_ms = t;
    }
    s_gps.pps_edges       = s_pps_edges;
    s_gps.pps_interval_us = (uint32_t)s_pps_interval_us;
    xSemaphoreGive(s_mux);
}

// A receiver that has gone quiet must stop claiming satellites, or the display
// would keep showing a healthy count long after the antenna was pulled.
static void age_out(void) {
    uint32_t t = now_ms();
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_gps.present && (t - s_gps.last_rx_ms) > PRESENT_HOLD) {
        ESP_LOGW(TAG, "no valid sentence for %d s — receiver considered absent",
                 PRESENT_HOLD / 1000);
        uint32_t csum = s_gps.csum_errors;
        memset(&s_gps, 0, sizeof s_gps);
        s_gps.fix = NMEA_FIX_NONE;
        s_gps.csum_errors = csum;
    }
    xSemaphoreGive(s_mux);
}

static void gps_task(void *arg) {
    const board_t *b = board_get();
    nmea_state_t n;
    nmea_reset(&n);

    char line[LINE_MAX];
    int  li = 0;
    bool overflow = false;
    uint8_t buf[256];

    ESP_LOGI(TAG, "listening on UART1 rx=%d tx=%d pps=%d @ %d baud",
             b->gps_rx, b->gps_tx, b->gps_pps, GPS_BAUD);

    for (;;) {
        int got = uart_read_bytes(GPS_UART, buf, sizeof buf, pdMS_TO_TICKS(200));
        for (int i = 0; i < got; i++) {
            char ch = (char)buf[i];
            if (ch == '\r' || ch == '\n') {
                if (!overflow && li > 6) {
                    line[li] = 0;
                    if (nmea_checksum_ok(line)) {
                        nmea_line(&n, line);
                        publish(&n, n.have_pos && n.rmc_valid);
                    } else {
                        xSemaphoreTake(s_mux, portMAX_DELAY);
                        s_gps.csum_errors++;
                        xSemaphoreGive(s_mux);
                    }
                }
                li = 0;
                overflow = false;
            } else if (li < LINE_MAX - 1) {
                line[li++] = ch;
            } else {
                overflow = true;    // drop the whole oversized line, not a prefix
            }
        }
        if (got <= 0) age_out();
    }
}

void gps_start(void) {
    const board_t *b = board_get();
    if (b->gps_rx < 0) return;               // board has no GNSS — stay inert

    if (!s_mux) s_mux = xSemaphoreCreateMutex();
    memset(&s_gps, 0, sizeof s_gps);
    s_gps.fix = NMEA_FIX_NONE;

    uart_config_t uc = {
        .baud_rate = GPS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(GPS_UART, RX_BUF, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GPS_UART, &uc));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART, b->gps_tx, b->gps_rx,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    if (b->gps_pps >= 0) {
        gpio_config_t in = {
            .pin_bit_mask = 1ULL << b->gps_pps,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = 0, .pull_down_en = 0,
            .intr_type = GPIO_INTR_POSEDGE,
        };
        gpio_config(&in);
        static bool isr_installed;
        if (!isr_installed) { gpio_install_isr_service(0); isr_installed = true; }
        gpio_isr_handler_add(b->gps_pps, pps_isr, NULL);
        // gpio_config() above already enables the interrupt because intr_type is
        // not DISABLE. Configuring with DISABLE and then calling
        // gpio_set_intr_type() leaves it OFF and makes a good wire look dead —
        // that cost an hour on the bench (LEARNING.md 2026-07-25).
        gpio_intr_enable(b->gps_pps);
    }

    xTaskCreate(gps_task, "gps", 4096, NULL, 5, NULL);
}
