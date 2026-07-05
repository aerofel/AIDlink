// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// On-device DNS forwarder: AP/USB clients are handed their gateway IP as DNS;
// this relays each query to the live uplink resolver with NAT-style transaction-id
// remapping so replies never collide across clients. Pure helpers (dnsfwd_util.c)
// are host-testable; the relay task (dnsfwd.c) is ESP-IDF.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define DNSFWD_SLOTS 16

typedef struct {
    uint8_t  cip[4];   // client IP (network order bytes)
    uint16_t cport;    // client UDP port (network order)
    uint16_t oid;      // client's original DNS transaction id (host order)
    uint16_t sid;      // our remapped transaction id (host order)
    uint32_t t0;       // millis when queued
    bool     used;
} dnsfwd_pend_t;

// Pure helpers (host-testable). sid encodes the slot in the top nibble and a
// rolling sequence in the low 12 bits so a reply can be routed back to its slot
// and stale replies after slot reuse are rejected.
uint16_t dnsfwd_make_sid(int slot, uint16_t *seq);
int dnsfwd_slot_of(uint16_t sid);

// Start the UDP :53 relay task. sta_netif is the STA esp_netif (passed as void*
// to keep this header ESP-IDF-free); client_dns_override forces an upstream
// resolver when non-empty (else the live STA resolver is used).
void dnsfwd_start(void *sta_netif, const char *client_dns_override);
