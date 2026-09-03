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

// Concurrent in-flight queries. A single page load fans out one query per
// distinct hostname and a phone/laptop stub fires them in a burst, so 16 was
// thin enough that the oldest-eviction path in dnsfwd_task discarded queries
// that were still legitimately in flight over a multi-second satellite RTT.
// Must stay equal to DNSFWD_SLOT_MASK + 1 (asserted in dnsfwd_util.c) — the
// slot decoded from a reply indexes the pending array directly.
#define DNSFWD_SLOTS      32
#define DNSFWD_SLOT_MASK  0x1F
#define DNSFWD_SLOT_SHIFT 11

// How long a pending query holds its slot before we give up on the uplink.
// MUST exceed the real uplink RTT: on the Viasat satellite link this is
// measured at 1-10 s (see the inet probe in netcore.c, which allows 12 s), and
// the previous 3 s budget *discarded valid replies* — the slot was already
// freed, so the reply was dropped and the client had to retry, paying another
// full satellite round trip to get an answer we had already received.
#define DNSFWD_TIMEOUT_MS 9000

// DNS QTYPEs we care about.
#define DNS_TYPE_A     1
#define DNS_TYPE_AAAA  28
#define DNS_TYPE_HTTPS 65   // SVCB-family HTTPS RR (RFC 9460)

// Reply cache. Every uplink lookup costs a full satellite round trip (measured
// 610-835 ms), and it is paid PER DEVICE: the phones and tablets on the AP each
// keep their own OS-level cache, so nothing is shared between them today. One
// small cache on the AID serves all of them. Sized to stay comfortably inside
// internal SRAM on the non-PSRAM ESP32 board too: 24 * ~328 B is under 8 KB of
// .bss, and a name or reply that does not fit is simply not cached.
#define DNSFWD_CACHE_N     24    // entries
#define DNSFWD_CACHE_QMAX  64    // max question bytes used as the key
#define DNSFWD_CACHE_RMAX  256   // max reply bytes stored (a 1-name A reply is ~60-120)
#define DNSFWD_CACHE_MIN_S 5     // clamp: never cache for less than this
#define DNSFWD_CACHE_MAX_S 300   // clamp: nor longer, so the link's own DNS can move

typedef struct {
    uint8_t  cip[4];   // client IP (network order bytes)
    uint16_t cport;    // client UDP port (network order)
    uint16_t oid;      // client's original DNS transaction id (host order)
    uint16_t sid;      // our remapped transaction id (host order)
    uint32_t t0;       // millis when queued
    bool     used;
    // The question we asked upstream, kept so the reply can be cached under it.
    // Taken from the query rather than re-parsed out of the reply, so the strict
    // "queries only" contract of dnsfwd_question_end() stays intact.
    uint8_t  q[DNSFWD_CACHE_QMAX];
    uint16_t qlen;
} dnsfwd_pend_t;

// Cache counters, for /status.
void dnsfwd_cache_stats(uint32_t *hits, uint32_t *misses);

// Pure helpers (host-testable). sid encodes the slot in the top DNSFWD_SLOT_MASK
// bits and a rolling sequence in the low DNSFWD_SLOT_SHIFT bits, so a reply can
// be routed back to its slot and stale replies after slot reuse are rejected.
uint16_t dnsfwd_make_sid(int slot, uint16_t *seq);
int dnsfwd_slot_of(uint16_t sid);

// QTYPE of the first question, or -1 if `msg` is not a parseable single-question
// query (a response, multi-question, truncated, or a compressed QNAME).
int dnsfwd_qtype(const uint8_t *msg, int len);

// True for query types we answer ourselves instead of paying an uplink round
// trip. See dnsfwd_util.c for why each type is safe to answer locally.
bool dnsfwd_answer_locally(int qtype);

// Rewrite the query in `msg` (len bytes, buffer capacity `cap`) in place into an
// empty NOERROR reply — NODATA, meaning "this name exists, it just has no record
// of this type". Returns the reply length, or -1 if the query is not rewritable.
int dnsfwd_make_nodata(uint8_t *msg, int len, int cap);

// Offset just past the first question (header + QNAME + QTYPE + QCLASS), or -1
// if `msg` is not a well-formed single-question query. The bytes from offset 12
// to here are the cache key.
int dnsfwd_question_end(const uint8_t *msg, int len);

// Compare two question sections. DNS names are case-insensitive on the wire, so
// "WWW.Example.com" and "www.example.com" must hit the same cache entry.
bool dnsfwd_question_eq(const uint8_t *a, int alen, const uint8_t *b, int blen);

// Smallest TTL (seconds) across the answer records of a reply, or -1 when there
// are none or the message cannot be walked safely. Compression pointers are
// skipped, never followed — we only need to step over names, and following an
// attacker-supplied pointer is how these parsers loop forever.
int dnsfwd_min_ttl(const uint8_t *msg, int len);

// Whether a reply may be cached at all: it must be a response, untruncated, and
// carry a definite result (NOERROR or NXDOMAIN). SERVFAIL/REFUSED must never be
// cached — on this link they are usually a transient satellite drop, and pinning
// one for minutes would turn a blip into an outage.
bool dnsfwd_cacheable(const uint8_t *msg, int len);

// Start the UDP :53 relay task. sta_netif is the STA esp_netif (passed as void*
// to keep this header ESP-IDF-free); client_dns_override forces an upstream
// resolver when non-empty (else the live STA resolver is used).
void dnsfwd_start(void *sta_netif, const char *client_dns_override);
