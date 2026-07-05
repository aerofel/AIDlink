// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "pos.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static pos_state_t s_pos;
static SemaphoreHandle_t s_mux;

void pos_init(void) {
    if (!s_mux) s_mux = xSemaphoreCreateMutex();
    memset(&s_pos, 0, sizeof s_pos);
}

void pos_get(pos_state_t *out) {
    if (!s_mux) { *out = s_pos; return; }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_pos;
    xSemaphoreGive(s_mux);
}

void pos_set(const pos_state_t *in) {
    if (!s_mux) { s_pos = *in; return; }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_pos = *in;
    xSemaphoreGive(s_mux);
}

void pos_mark_stale(void) {
    if (!s_mux) { s_pos.valid = false; s_pos.service_avail = false; return; }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_pos.valid = false;
    s_pos.service_avail = false;
    xSemaphoreGive(s_mux);
}
