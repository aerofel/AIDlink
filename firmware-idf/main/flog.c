// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "flog.h"
#include "flog_core.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "flog";

static const esp_partition_t *s_part;
static SemaphoreHandle_t s_mux;
static int      s_nsec;      // sectors in the partition
static int      s_sec = -1;  // current write sector (-1 = none yet)
static int      s_off;       // append offset within the current sector payload
static uint32_t s_seq;       // seq of the current sector

bool flog_available(void) { return s_part != NULL; }

// Start a fresh sector at index idx with sequence seq (erase + header).
static bool sector_begin(int idx, uint32_t seq) {
    if (esp_partition_erase_range(s_part, (size_t)idx * FLOG_SEC_SIZE, FLOG_SEC_SIZE) != ESP_OK)
        return false;
    flog_hdr_t h = { .magic = FLOG_MAGIC, .rsv = 0, .seq = seq };
    if (esp_partition_write(s_part, (size_t)idx * FLOG_SEC_SIZE, &h, sizeof h) != ESP_OK)
        return false;
    s_sec = idx; s_off = 0; s_seq = seq;
    return true;
}

// Append raw bytes, rolling to the next sector when the line doesn't fit.
// Called with the mutex held.
static void append_locked(const char *data, int len) {
    if (len > FLOG_PAY_SIZE) len = FLOG_PAY_SIZE;   // never split one line across sectors
    if (s_sec < 0 || !flog_fits(s_off, len, FLOG_PAY_SIZE)) {
        int nxt = s_sec < 0 ? 0 : flog_next_sector(s_sec, s_nsec);
        if (!sector_begin(nxt, s_seq + 1)) return;
    }
    size_t at = (size_t)s_sec * FLOG_SEC_SIZE + FLOG_HDR_SIZE + s_off;
    if (esp_partition_write(s_part, at, data, len) == ESP_OK) s_off += len;
}

static void boot_marker(void) {
    static const char *rr[] = { "unknown", "poweron", "ext", "sw", "panic", "int_wdt",
                                "task_wdt", "wdt", "deepsleep", "brownout", "sdio",
                                "usb", "jtag", "efuse", "pwr_glitch", "cpu_lockup" };
    int r = (int)esp_reset_reason();
    if (r < 0 || r >= (int)(sizeof rr / sizeof rr[0])) r = 0;
    char line[96];
    snprintf(line, sizeof line, "=== boot: reset=%s ===", rr[r]);
    flog_line(line);
}

void flog_init(void) {
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "flog");
    if (!s_part) { ESP_LOGW(TAG, "no flog partition — persistent log disabled"); return; }
    s_mux = xSemaphoreCreateMutex();
    s_nsec = s_part->size / FLOG_SEC_SIZE;

    // Find the write head: read every sector header, pick the highest seq,
    // then locate the first unwritten byte inside that sector's payload.
    flog_hdr_t *hdrs = malloc(sizeof(flog_hdr_t) * s_nsec);
    if (!hdrs) { s_part = NULL; return; }
    for (int i = 0; i < s_nsec; i++)
        if (esp_partition_read(s_part, (size_t)i * FLOG_SEC_SIZE, &hdrs[i], sizeof hdrs[i]) != ESP_OK)
            hdrs[i].magic = 0;
    s_sec = flog_head_sector(hdrs, s_nsec);
    free(hdrs);

    if (s_sec >= 0) {
        flog_hdr_t h;
        esp_partition_read(s_part, (size_t)s_sec * FLOG_SEC_SIZE, &h, sizeof h);
        s_seq = h.seq;
        static uint8_t pay[FLOG_PAY_SIZE];   // 4 KB scan buffer, boot-only use
        if (esp_partition_read(s_part, (size_t)s_sec * FLOG_SEC_SIZE + FLOG_HDR_SIZE,
                               pay, sizeof pay) == ESP_OK)
            s_off = flog_append_off(pay, sizeof pay);
        else s_off = FLOG_PAY_SIZE;
    } else s_seq = 0;

    ESP_LOGI(TAG, "flog on %s: %d sectors, head sec=%d off=%d seq=%lu",
             s_part->label, s_nsec, s_sec, s_off, (unsigned long)s_seq);
    boot_marker();
}

void flog_line(const char *line) {
    if (!s_part || !line) return;
    // Timestamp: uptime always; UTC hh:mm:ss once the clock is disciplined
    // (poller sets it from the HTTP Date header, so post-flight analysis can
    // correlate lines with flight time).
    char out[256];
    uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    time_t t = time(NULL);
    int o;
    if (t > 1700000000) {
        struct tm tm; gmtime_r(&t, &tm);
        o = snprintf(out, sizeof out, "[%lu %02d:%02d:%02dZ] ",
                     (unsigned long)up_s, tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        o = snprintf(out, sizeof out, "[%lu] ", (unsigned long)up_s);
    }
    o += snprintf(out + o, sizeof out - o - 1, "%s", line);
    if (o > (int)sizeof out - 2) o = (int)sizeof out - 2;
    out[o++] = '\n'; out[o] = 0;

    xSemaphoreTake(s_mux, portMAX_DELAY);
    append_locked(out, o);
    xSemaphoreGive(s_mux);
}

void flog_dump(void (*emit)(const char *chunk, int len, void *ctx), void *ctx) {
    if (!s_part) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    // Emit sectors in ascending seq order (== chronological write order).
    // O(n²) selection over ≤128 headers — trivial at dump time.
    static uint8_t pay[FLOG_PAY_SIZE];
    uint32_t last_seq = 0;
    for (int pass = 0; pass < s_nsec; pass++) {
        int pick = -1; uint32_t pick_seq = 0;
        for (int i = 0; i < s_nsec; i++) {
            flog_hdr_t h;
            if (esp_partition_read(s_part, (size_t)i * FLOG_SEC_SIZE, &h, sizeof h) != ESP_OK) continue;
            if (h.magic != FLOG_MAGIC || h.rsv != 0 || h.seq <= last_seq) continue;
            if (pick < 0 || h.seq < pick_seq) { pick = i; pick_seq = h.seq; }
        }
        if (pick < 0) break;
        last_seq = pick_seq;
        if (esp_partition_read(s_part, (size_t)pick * FLOG_SEC_SIZE + FLOG_HDR_SIZE,
                               pay, sizeof pay) != ESP_OK) continue;
        int used = flog_append_off(pay, sizeof pay);
        if (used > 0) emit((const char *)pay, used, ctx);
    }
    xSemaphoreGive(s_mux);
}

void flog_clear(void) {
    if (!s_part) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    esp_partition_erase_range(s_part, 0, s_part->size);
    s_sec = -1; s_off = 0; s_seq = 0;
    xSemaphoreGive(s_mux);
    boot_marker();
}

uint32_t flog_used_bytes(void) {
    if (!s_part) return 0;
    uint32_t used = 0;
    for (int i = 0; i < s_nsec; i++) {
        flog_hdr_t h;
        if (esp_partition_read(s_part, (size_t)i * FLOG_SEC_SIZE, &h, sizeof h) != ESP_OK) continue;
        if (h.magic == FLOG_MAGIC && h.rsv == 0) used += FLOG_SEC_SIZE;
    }
    return used;
}
