// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Pure DNS txn-id remap + local-answer helpers — no ESP-IDF dependencies,
// host-unit-testable.
#include "dnsfwd.h"

// The slot decoded from a reply indexes s_pend[] directly, so the encodable
// slot range and the array size must not drift apart.
_Static_assert(DNSFWD_SLOTS == DNSFWD_SLOT_MASK + 1,
               "DNSFWD_SLOTS must match the slot bits encoded in the txn id");

uint16_t dnsfwd_make_sid(int slot, uint16_t *seq) {
    uint16_t seq_mask = (uint16_t)((1u << DNSFWD_SLOT_SHIFT) - 1);
    return (uint16_t)(((slot & DNSFWD_SLOT_MASK) << DNSFWD_SLOT_SHIFT) |
                      ((*seq)++ & seq_mask));
}

int dnsfwd_slot_of(uint16_t sid) {
    return (sid >> DNSFWD_SLOT_SHIFT) & DNSFWD_SLOT_MASK;
}

// Offset just past the first question's QTYPE+QCLASS, or -1 if `msg` is not a
// well-formed single-question query. Everything here is attacker-reachable (any
// client on the AP can send arbitrary bytes to :53), so every step is bounded.
static int question_end(const uint8_t *msg, int len) {
    if (!msg || len < 12) return -1;
    if (msg[2] & 0x80) return -1;                    // QR set -> a response
    if (((msg[4] << 8) | msg[5]) != 1) return -1;    // QDCOUNT != 1
    int i = 12;
    bool terminated = false;
    while (i < len) {
        uint8_t l = msg[i];
        if (l == 0) { i++; terminated = true; break; }
        if (l & 0xC0) return -1;      // compression pointer/reserved: invalid in a QNAME
        i += 1 + l;
    }
    if (!terminated) return -1;       // ran off the end mid-QNAME
    if (i + 4 > len) return -1;       // no room for QTYPE + QCLASS
    return i + 4;
}

int dnsfwd_qtype(const uint8_t *msg, int len) {
    int e = question_end(msg, len);
    if (e < 0) return -1;
    return (msg[e - 4] << 8) | msg[e - 3];
}

bool dnsfwd_answer_locally(int qtype) {
    // AAAA: the AID cannot forward IPv6 at all (CONFIG_LWIP_IPV6_FORWARD is off
    // and we send no Router Advertisement, so a client only ever gets a
    // link-local address and no v6 default route). Any AAAA answer we relayed
    // would name an address the client provably cannot reach, so the whole
    // round trip is waste. Answering NODATA immediately also collapses the
    // getaddrinfo() A/AAAA race to the A lookup alone.
    //
    // HTTPS (SVCB): costs a third round trip per hostname and only carries
    // optional hints (ECH, h3/ALPN). Declining it keeps clients on TCP/h2,
    // which is the better transport over a lossy high-RTT satellite link.
    // Drop this clause if ECH or HTTP/3 is ever wanted on the uplink.
    return qtype == DNS_TYPE_AAAA || qtype == DNS_TYPE_HTTPS;
}

int dnsfwd_question_end(const uint8_t *msg, int len) {
    return question_end(msg, len);
}

static uint8_t lower(uint8_t c) {
    return (c >= 'A' && c <= 'Z') ? (uint8_t)(c - 'A' + 'a') : c;
}

bool dnsfwd_question_eq(const uint8_t *a, int alen, const uint8_t *b, int blen) {
    if (!a || !b || alen <= 0 || alen != blen) return false;
    for (int i = 0; i < alen; i++)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

// Step over one (possibly compressed) name. Returns the offset just past it, or
// -1. A compression pointer is only STEPPED OVER, never followed: we never need
// the name's text, and following attacker-supplied pointers is how DNS parsers
// are made to loop.
static int skip_name(const uint8_t *m, int len, int i) {
    while (i >= 0 && i < len) {
        uint8_t l = m[i];
        if ((l & 0xC0) == 0xC0) return (i + 2 <= len) ? i + 2 : -1;
        if (l == 0) return i + 1;
        if (l & 0xC0) return -1;          // reserved label type
        i += 1 + l;
    }
    return -1;
}

int dnsfwd_min_ttl(const uint8_t *msg, int len) {
    if (!msg || len < 12) return -1;
    int qd = (msg[4] << 8) | msg[5];
    int an = (msg[6] << 8) | msg[7];
    if (an <= 0) return -1;

    int i = 12;
    for (int q = 0; q < qd; q++) {              // step over the question section
        i = skip_name(msg, len, i);
        if (i < 0 || i + 4 > len) return -1;
        i += 4;                                 // QTYPE + QCLASS
    }

    long best = -1;
    for (int a = 0; a < an; a++) {
        i = skip_name(msg, len, i);
        if (i < 0 || i + 10 > len) return -1;   // TYPE+CLASS+TTL+RDLENGTH
        // TTL is a 32-bit signed value; RFC 2181 says treat a negative one as 0.
        long ttl = ((long)msg[i + 4] << 24) | ((long)msg[i + 5] << 16) |
                   ((long)msg[i + 6] << 8)  |  (long)msg[i + 7];
        ttl &= 0x7FFFFFFFL;
        int rdlen = (msg[i + 8] << 8) | msg[i + 9];
        i += 10 + rdlen;
        if (i > len) return -1;                 // rdata runs past the message
        if (best < 0 || ttl < best) best = ttl;
    }
    return (int)best;
}

bool dnsfwd_cacheable(const uint8_t *msg, int len) {
    if (!msg || len < 12) return false;
    if (!(msg[2] & 0x80)) return false;         // must be a response
    if (msg[2] & 0x02) return false;            // truncated -> the client must retry over TCP
    int rcode = msg[3] & 0x0F;
    // Only definite results. SERVFAIL/REFUSED on this link are usually a
    // transient satellite drop; caching one would turn a blip into an outage.
    return rcode == 0 || rcode == 3;            // NOERROR or NXDOMAIN
}

int dnsfwd_make_nodata(uint8_t *msg, int len, int cap) {
    int e = question_end(msg, len);
    if (e < 0 || e > cap) return -1;

    // QR=1, keep OPCODE and echo RD; clear AA (we are not authoritative) and TC.
    msg[2] = (uint8_t)((msg[2] & 0x79) | 0x80);
    // RA=1, Z/AD/CD cleared, RCODE=0 (NOERROR). NOERROR with ANCOUNT=0 is
    // NODATA: "no record of this type". It must NOT be NXDOMAIN, which would
    // deny the whole name and can poison the A lookup for the same hostname.
    msg[3] = 0x80;
    msg[6] = msg[7] = 0;      // ANCOUNT = 0
    msg[8] = msg[9] = 0;      // NSCOUNT = 0
    // ARCOUNT = 0 drops any EDNS0 OPT the client sent. A reply without OPT is
    // legal (it reads as "no EDNS support here") and stub resolvers accept it.
    msg[10] = msg[11] = 0;

    return e;                 // header + the single question, no records
}
