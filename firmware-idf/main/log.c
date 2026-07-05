// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static char s_buf[LOG_N][LOG_LINE];
static int  s_head;                 // index of the oldest line
static int  s_count;                // number of stored lines
static bool s_enable;
static SemaphoreHandle_t s_mux;

void log_init(void) {
    if (!s_mux) s_mux = xSemaphoreCreateMutex();
    s_head = s_count = 0;
}

void log_set_enable(bool en) { s_enable = en; }
bool log_enabled(void) { return s_enable; }

void logln(const char *fmt, ...) {
    if (!s_enable || !s_mux) return;
    char line[LOG_LINE];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    xSemaphoreTake(s_mux, portMAX_DELAY);
    int idx = (s_head + s_count) % LOG_N;
    if (s_count < LOG_N) s_count++;
    else s_head = (s_head + 1) % LOG_N;   // full -> overwrite oldest
    strlcpy(s_buf[idx], line, LOG_LINE);
    xSemaphoreGive(s_mux);
}

int log_foreach(void (*cb)(const char *line, void *ctx), void *ctx) {
    if (!s_mux) return 0;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int n = s_count;
    for (int i = 0; i < n; i++) cb(s_buf[(s_head + i) % LOG_N], ctx);
    xSemaphoreGive(s_mux);
    return n;
}
