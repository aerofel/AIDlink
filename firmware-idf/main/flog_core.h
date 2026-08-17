// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Pure sector-ring math for the persistent flash log (flog.c is the ESP-IDF
// glue). A flog partition is a ring of 4 KB sectors; each starts with an
// 8-byte header (magic + monotonically increasing sequence number) followed by
// plain text lines. Erased flash reads 0xFF, which doubles as the "no data
// yet" marker inside a sector payload. No ESP types — host-unit-testable.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define FLOG_SEC_SIZE 4096
#define FLOG_MAGIC    0xF106
#define FLOG_HDR_SIZE ((int)sizeof(flog_hdr_t))
#define FLOG_PAY_SIZE (FLOG_SEC_SIZE - FLOG_HDR_SIZE)

typedef struct {
    uint16_t magic;   // FLOG_MAGIC when the sector holds log data
    uint16_t rsv;
    uint32_t seq;     // strictly increasing across sectors: write order
} flog_hdr_t;

// Index of the sector holding the write head (valid magic, highest seq), or
// -1 when no sector has ever been written (fresh/erased partition).
int flog_head_sector(const flog_hdr_t *hdrs, int n);

// Next sector in the ring after cur.
int flog_next_sector(int cur, int n);

// First unwritten byte (0xFF) in a sector payload; == paylen when full.
int flog_append_off(const uint8_t *payload, int paylen);

// Whether len bytes fit at offset off inside a payload of paylen bytes.
bool flog_fits(int off, int len, int paylen);
