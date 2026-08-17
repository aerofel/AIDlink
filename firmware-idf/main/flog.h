// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Persistent flash log (spec 2026-08-13): a circular text log on the dedicated
// `flog` data partition, so a flight's diagnostic trail survives power-off and
// can be pulled after landing via the auth-gated /flog endpoint. Fed by the
// same logln() stream as the RAM ring (via log.c's sink hook), gated by the
// same "Live capture" toggle. Sector math lives in flog_core.c (host-tested).
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Locate the partition, find the write head, and append a boot marker line
// (build + reset reason). Safe to call when the partition is absent (all
// other calls become no-ops; flog_available() reports false).
void flog_init(void);

bool flog_available(void);

// Append one text line (timestamped here: uptime always, UTC when the clock
// has been disciplined). Thread-safe. No-op when unavailable.
void flog_line(const char *line);

// Stream the whole log oldest->newest as text chunks into emit(). Thread-safe.
void flog_dump(void (*emit)(const char *chunk, int len, void *ctx), void *ctx);

// Erase everything and restart with a fresh boot marker.
void flog_clear(void);

// Bytes currently stored (approximate, for the UI).
uint32_t flog_used_bytes(void);
