// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Device traffic log: a small in-RAM ring buffer of recent ADBP/HTTP events,
// surfaced by the web UI's /log endpoint. Capture is gated by log_set_enable().
#pragma once
#include <stdbool.h>

#define LOG_N 90        // ring capacity (lines) — matches v9
#define LOG_LINE 200    // max chars per line

void log_init(void);
void log_set_enable(bool en);
bool log_enabled(void);

// Append a line (dropped if capture disabled). printf-style.
void logln(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Iterate stored lines oldest->newest, invoking cb for each. Returns line count.
int log_foreach(void (*cb)(const char *line, void *ctx), void *ctx);
