// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Device traffic log: a small in-RAM ring buffer of recent ADBP/HTTP events,
// surfaced by the web UI's /log endpoint. Capture is gated by log_set_enable().
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LOG_N 90        // ring capacity (lines) — matches v9
#define LOG_LINE 200    // max chars per line

void log_init(void);
void log_set_enable(bool en);
bool log_enabled(void);

// Append a line: always to the flash sink (when set), to the RAM ring only
// while capture is enabled. printf-style.
void logln(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// RAM ring only, never flash: for per-request traffic (every ADBP poll, every
// HTTP hit). At an EFB's 1 Hz those lines would cycle the 512 KB flash log in
// ~2 h and overwrite the evidence of the rest of the flight.
void logln_ram(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Optional secondary sink: every captured line is also handed to sink() after
// being stored in the ring (e.g. flog.c's persistent flash log). Called
// outside the ring mutex, from the logging task's context.
void log_set_sink(void (*sink)(const char *line));

// Iterate stored lines oldest->newest, invoking cb for each (without holding
// the log lock). cb returns false to stop early. Returns lines emitted.
int log_foreach(bool (*cb)(const char *line, void *ctx), void *ctx);
