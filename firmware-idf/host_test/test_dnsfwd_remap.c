// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
// Host unit test for the DNS txn-id remap + local-answer helpers
// (build with clang, no ESP-IDF).
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "dnsfwd.h"

static int n_assert;
#define CHECK(x) do { assert(x); n_assert++; } while (0)

// Build a single-question query for "www.example.com" of type `qtype` into buf.
// Returns the length. rd = set the Recursion Desired bit (what a real stub sends).
static int mkquery(uint8_t *b, int qtype, bool rd) {
    static const uint8_t qname[] = {
        3, 'w', 'w', 'w', 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0
    };
    b[0] = 0x12; b[1] = 0x34;               // txn id
    b[2] = rd ? 0x01 : 0x00; b[3] = 0x00;   // flags: QR=0, RD
    b[4] = 0; b[5] = 1;                     // QDCOUNT = 1
    b[6] = 0; b[7] = 0;                     // ANCOUNT
    b[8] = 0; b[9] = 0;                     // NSCOUNT
    b[10] = 0; b[11] = 0;                   // ARCOUNT
    memcpy(b + 12, qname, sizeof qname);
    int i = 12 + (int)sizeof qname;
    b[i++] = (uint8_t)(qtype >> 8); b[i++] = (uint8_t)qtype;   // QTYPE
    b[i++] = 0; b[i++] = 1;                                    // QCLASS = IN
    return i;
}

int main(void) {
    // ---- txn-id remap ------------------------------------------------------
    uint16_t seq = 0;
    CHECK(dnsfwd_slot_of(dnsfwd_make_sid(5, &seq)) == 5);

    // slot stable while seq varies -> two ids for the same slot differ
    seq = 0x07FE;
    uint16_t a = dnsfwd_make_sid(3, &seq);
    uint16_t b = dnsfwd_make_sid(3, &seq);
    CHECK(dnsfwd_slot_of(a) == 3);
    CHECK(dnsfwd_slot_of(b) == 3);
    CHECK(a != b);

    // EVERY slot must round-trip, and must stay inside the pending array --
    // this is the invariant that a widened slot field could silently break.
    for (int s = 0; s < DNSFWD_SLOTS; s++) {
        seq = (uint16_t)(s * 37);
        int got = dnsfwd_slot_of(dnsfwd_make_sid(s, &seq));
        CHECK(got == s);
        CHECK(got >= 0 && got < DNSFWD_SLOTS);
    }
    // and a hostile/rolled sid can never index past the array either
    for (int i = 0; i < 0x10000; i += 97)
        CHECK(dnsfwd_slot_of((uint16_t)i) >= 0 && dnsfwd_slot_of((uint16_t)i) < DNSFWD_SLOTS);

    // ---- qtype parsing ----------------------------------------------------
    uint8_t q[512];
    int len = mkquery(q, DNS_TYPE_A, true);
    CHECK(dnsfwd_qtype(q, len) == DNS_TYPE_A);
    len = mkquery(q, DNS_TYPE_AAAA, true);
    CHECK(dnsfwd_qtype(q, len) == DNS_TYPE_AAAA);
    len = mkquery(q, DNS_TYPE_HTTPS, true);
    CHECK(dnsfwd_qtype(q, len) == DNS_TYPE_HTTPS);

    // malformed / hostile inputs must be rejected, never forwarded blind
    CHECK(dnsfwd_qtype(NULL, 40) == -1);
    CHECK(dnsfwd_qtype(q, 0) == -1);
    CHECK(dnsfwd_qtype(q, 11) == -1);              // shorter than a header
    len = mkquery(q, DNS_TYPE_AAAA, true);
    CHECK(dnsfwd_qtype(q, len - 1) == -1);         // truncated mid QTYPE/QCLASS
    CHECK(dnsfwd_qtype(q, 13) == -1);              // truncated inside QNAME
    q[2] |= 0x80;                                  // QR set -> a response, not a query
    CHECK(dnsfwd_qtype(q, len) == -1);
    len = mkquery(q, DNS_TYPE_AAAA, true);
    q[5] = 2;                                      // QDCOUNT = 2
    CHECK(dnsfwd_qtype(q, len) == -1);
    len = mkquery(q, DNS_TYPE_AAAA, true);
    q[12] = 0xC0;                                  // compression pointer in a QNAME
    CHECK(dnsfwd_qtype(q, len) == -1);

    // ---- which types we answer without touching the uplink ----------------
    CHECK(dnsfwd_answer_locally(DNS_TYPE_AAAA));
    CHECK(dnsfwd_answer_locally(DNS_TYPE_HTTPS));
    CHECK(!dnsfwd_answer_locally(DNS_TYPE_A));
    CHECK(!dnsfwd_answer_locally(15));             // MX must still be forwarded
    CHECK(!dnsfwd_answer_locally(-1));             // unparseable must be forwarded

    // ---- NODATA synthesis -------------------------------------------------
    len = mkquery(q, DNS_TYPE_AAAA, true);
    int rlen = dnsfwd_make_nodata(q, len, (int)sizeof q);
    CHECK(rlen == len);                            // header + the one question
    CHECK(q[0] == 0x12 && q[1] == 0x34);           // client's txn id preserved
    CHECK((q[2] & 0x80) != 0);                     // QR = response
    CHECK((q[2] & 0x01) != 0);                     // RD echoed back
    CHECK((q[2] & 0x04) == 0);                     // not authoritative
    CHECK((q[2] & 0x02) == 0);                     // not truncated
    CHECK((q[3] & 0x80) != 0);                     // RA = recursion available
    CHECK((q[3] & 0x0F) == 0);                     // RCODE = NOERROR, *not* NXDOMAIN
    CHECK(q[4] == 0 && q[5] == 1);                 // question still there
    CHECK(q[6] == 0 && q[7] == 0);                 // ANCOUNT = 0 -> NODATA
    CHECK(q[8] == 0 && q[9] == 0);                 // NSCOUNT = 0
    CHECK(q[10] == 0 && q[11] == 0);               // ARCOUNT = 0
    // the question bytes must be byte-identical to what the client asked
    uint8_t ref[512];
    int reflen = mkquery(ref, DNS_TYPE_AAAA, true);
    CHECK(reflen == rlen);
    CHECK(memcmp(q + 12, ref + 12, (size_t)(rlen - 12)) == 0);

    // an EDNS0 OPT in the additional section is dropped, and the reply shrinks
    len = mkquery(q, DNS_TYPE_AAAA, true);
    q[11] = 1;                                     // ARCOUNT = 1
    q[len++] = 0; q[len++] = 0; q[len++] = 41;     // a stub OPT RR (root name, type 41)
    rlen = dnsfwd_make_nodata(q, len, (int)sizeof q);
    CHECK(rlen == len - 3);                        // OPT trimmed off
    CHECK(q[10] == 0 && q[11] == 0);

    // malformed input must not produce a reply
    len = mkquery(q, DNS_TYPE_AAAA, true);
    CHECK(dnsfwd_make_nodata(q, 11, (int)sizeof q) == -1);
    CHECK(dnsfwd_make_nodata(q, len, 12) == -1);   // no room for the question

    // ---- cache key: question extraction + case-insensitive compare ---------
    uint8_t q1[512], q2[512];
    int l1 = mkquery(q1, DNS_TYPE_A, true);
    int e1 = dnsfwd_question_end(q1, l1);
    CHECK(e1 == l1);                               // query is header + one question
    CHECK(dnsfwd_question_end(q1, 11) == -1);

    l1 = mkquery(q1, DNS_TYPE_A, true);
    int l2 = mkquery(q2, DNS_TYPE_A, true);
    q2[13] = 'W'; q2[14] = 'W'; q2[15] = 'W';      // "WWW.example.com"
    CHECK(dnsfwd_question_eq(q1 + 12, l1 - 12, q2 + 12, l2 - 12));   // case-insensitive
    q2[16] = 8;                                    // corrupt a label length
    CHECK(!dnsfwd_question_eq(q1 + 12, l1 - 12, q2 + 12, l2 - 12));
    l2 = mkquery(q2, DNS_TYPE_AAAA, true);         // same name, different QTYPE
    CHECK(!dnsfwd_question_eq(q1 + 12, l1 - 12, q2 + 12, l2 - 12));
    CHECK(!dnsfwd_question_eq(q1 + 12, l1 - 12, q2 + 12, l2 - 13));  // length differs

    // ---- TTL extraction ---------------------------------------------------
    // Build a reply: query + one A answer (compressed name, TTL 120) .
    l1 = mkquery(q1, DNS_TYPE_A, true);
    int p = l1;
    q1[2] |= 0x80; q1[6] = 0; q1[7] = 1;           // response, ANCOUNT = 1
    q1[p++] = 0xC0; q1[p++] = 0x0C;                // name -> pointer to offset 12
    q1[p++] = 0; q1[p++] = 1;                      // TYPE A
    q1[p++] = 0; q1[p++] = 1;                      // CLASS IN
    q1[p++] = 0; q1[p++] = 0; q1[p++] = 0; q1[p++] = 120;   // TTL 120
    q1[p++] = 0; q1[p++] = 4;                      // RDLENGTH 4
    q1[p++] = 93; q1[p++] = 184; q1[p++] = 216; q1[p++] = 34;
    CHECK(dnsfwd_min_ttl(q1, p) == 120);
    CHECK(dnsfwd_min_ttl(q1, p - 1) == -1);        // truncated rdata rejected
    CHECK(dnsfwd_min_ttl(q1, 12) == -1);

    // a second answer with a SMALLER ttl must win
    int p2 = p;
    q1[7] = 2;                                     // ANCOUNT = 2
    q1[p2++] = 0xC0; q1[p2++] = 0x0C;
    q1[p2++] = 0; q1[p2++] = 1; q1[p2++] = 0; q1[p2++] = 1;
    q1[p2++] = 0; q1[p2++] = 0; q1[p2++] = 0; q1[p2++] = 30;   // TTL 30
    q1[p2++] = 0; q1[p2++] = 4;
    q1[p2++] = 1; q1[p2++] = 2; q1[p2++] = 3; q1[p2++] = 4;
    CHECK(dnsfwd_min_ttl(q1, p2) == 30);

    // a reply with no answers has no TTL to honour
    l1 = mkquery(q1, DNS_TYPE_A, true);
    q1[2] |= 0x80;
    CHECK(dnsfwd_min_ttl(q1, l1) == -1);

    // ---- what may be cached ----------------------------------------------
    l1 = mkquery(q1, DNS_TYPE_A, true);
    CHECK(!dnsfwd_cacheable(q1, l1));              // a query is not a reply
    q1[2] |= 0x80;
    CHECK(dnsfwd_cacheable(q1, l1));               // NOERROR response
    q1[3] = 3;                                     // NXDOMAIN
    CHECK(dnsfwd_cacheable(q1, l1));               // negative answers are cacheable
    q1[3] = 2;                                     // SERVFAIL
    CHECK(!dnsfwd_cacheable(q1, l1));              // transient on this link -> never cache
    q1[3] = 5;                                     // REFUSED
    CHECK(!dnsfwd_cacheable(q1, l1));
    q1[3] = 0; q1[2] |= 0x02;                      // TC set
    CHECK(!dnsfwd_cacheable(q1, l1));              // truncated: the client must retry
    CHECK(!dnsfwd_cacheable(q1, 11));

    printf("test_dnsfwd_remap: PASS (%d assertions)\n", n_assert);
    return 0;
}
