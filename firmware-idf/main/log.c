// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
#include <stdlib.h>

// 18 KB: in PSRAM where there is some (internal SRAM is the scarce resource
// on the S3 boards); allocated once in log_init(). NULL = ring unavailable.
static char (*s_buf)[LOG_LINE];
static int  s_head;                 // index of the oldest line
static int  s_count;                // number of stored lines
static uint32_t s_seq;              // lines ever stored; oldest held = s_seq - s_count
static bool s_enable;
static SemaphoreHandle_t s_mux;
static void (*s_sink)(const char *line);

void log_set_sink(void (*sink)(const char *line)) { s_sink = sink; }

void log_init(void) {
    if (!s_buf) {
#ifdef ESP_PLATFORM
        s_buf = heap_caps_malloc(LOG_N * LOG_LINE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
        if (!s_buf) s_buf = malloc(LOG_N * LOG_LINE);
    }
    if (!s_mux) s_mux = xSemaphoreCreateMutex();
    s_head = s_count = 0;
    s_seq = 0;
}

void log_set_enable(bool en) { s_enable = en; }
bool log_enabled(void) { return s_enable; }

// The flash sink ALWAYS receives the line: post-flight evidence must not hang
// on a "live capture" checkbox (unticking it used to silence the flash log
// until reboot). log_set_enable() only gates the RAM ring behind /log.
static void ring_put(const char *line) {
    if (!s_enable || !s_mux || !s_buf) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int idx = (s_head + s_count) % LOG_N;
    if (s_count < LOG_N) s_count++;
    else s_head = (s_head + 1) % LOG_N;   // full -> overwrite oldest
    strlcpy(s_buf[idx], line, LOG_LINE);
    s_seq++;
    xSemaphoreGive(s_mux);
}

void logln(const char *fmt, ...) {
    if (!s_mux) return;
    char line[LOG_LINE];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    ring_put(line);
    if (s_sink) s_sink(line);   // outside the ring mutex: the sink may hit flash
}

void logln_ram(const char *fmt, ...) {
    if (!s_enable || !s_mux) return;
    char line[LOG_LINE];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    ring_put(line);
}

// The mutex is held only to COPY one line, never across cb(): cb sends over
// the network, and every logln() caller (ADBP, the display under the LVGL lock,
// the poller, the Wi-Fi event task) waits on this mutex. A /log client that
// stopped reading used to freeze all of them for as long as httpd kept trying.
// Lines overwritten while we were sending are skipped, not repeated.
int log_foreach(bool (*cb)(const char *line, void *ctx), void *ctx) {
    if (!s_mux || !s_buf) return 0;
    char line[LOG_LINE];
    xSemaphoreTake(s_mux, portMAX_DELAY);
    uint32_t k = s_seq - (uint32_t)s_count, end = s_seq;
    xSemaphoreGive(s_mux);
    int n = 0;
    for (; (int32_t)(end - k) > 0; k++) {
        xSemaphoreTake(s_mux, portMAX_DELAY);
        uint32_t oldest = s_seq - (uint32_t)s_count;
        if ((int32_t)(k - oldest) < 0) k = oldest;          // overwritten meanwhile
        bool have = (int32_t)(end - k) > 0;                 // still inside the snapshot
        if (have) strlcpy(line, s_buf[(s_head + (k - oldest)) % LOG_N], sizeof line);
        xSemaphoreGive(s_mux);
        if (!have) break;
        n++;
        if (!cb(line, ctx)) break;                          // client gone: stop
    }
    return n;
}
