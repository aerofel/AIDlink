// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "memwatch.h"
#include "log.h"
#include "netcore.h"
#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#if CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC
#include "mbedtls/platform.h"
#endif

// Report once the pool is below this, then once per further MW_STEP drop.
#define MW_WARN_BELOW 16384u
#define MW_STEP        2048u

// Written by the failed-alloc hook, which can run in any context (incl. with
// the cache disabled), so: IRAM code, DRAM data, no logging, no locks, no
// __atomic builtins (-mdisable-hardware-atomics makes them library calls).
static volatile uint32_t s_fail_n, s_fail_size, s_fail_caps;
static const char *volatile s_fail_fn;
static uint32_t s_fail_logged;

static volatile uint32_t s_connects;
static uint32_t s_pool_logged = UINT32_MAX, s_all_logged = UINT32_MAX;

static void IRAM_ATTR on_alloc_failed(size_t size, uint32_t caps, const char *fn) {
    s_fail_size = size;
    s_fail_caps = caps;
    s_fail_fn = fn;
    s_fail_n++;   // plain increment: the S3 build has no hardware atomics, and a
                  // count lost to two cores failing at once is harmless here
}

#if CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC
// mbedTLS allocator: PSRAM FIRST, internal SRAM only as a fallback.
//
// Under MBEDTLS_DEFAULT_MEM_ALLOC every mbedTLS allocation of <= 4 KB
// (SPIRAM_MALLOC_ALWAYSINTERNAL) -- bignums, handshake state, cert parsing --
// went to internal SRAM first. The Viasat poller is a TLS client, and the
// Wi-Fi WPA2 handshakes use mbedTLS crypto too (ESP_WIFI_MBEDTLS_CRYPTO), so
// every reconnect or client join spent ~20 KB of the scarce internal heap that
// the Wi-Fi driver and lwIP need. EXTERNAL_MEM_ALLOC would fix that but has no
// fallback: SPIRAM_IGNORE_NOTFOUND lets a board boot without PSRAM, and TLS
// would then be impossible. This keeps the 2026-09-19 property (a handshake
// never fails just because one region is short) with the preference reversed.
// CUSTOM mode means IDF installs nothing: memwatch_init() registers these via
// mbedtls_platform_set_calloc_free(), before Wi-Fi (the first mbedTLS user)
// starts. heap_caps_free() frees from either region, so mixing is safe.
static void *tls_calloc(size_t n, size_t size) {
    return heap_caps_calloc_prefer(n, size, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void tls_free(void *ptr) { heap_caps_free(ptr); }
#endif

void memwatch_init(void) {
    heap_caps_register_failed_alloc_callback(on_alloc_failed);
#if CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC
    mbedtls_platform_set_calloc_free(tls_calloc, tls_free);
#endif
}

void memwatch_note_connect(void) { s_connects++; }   // poller task only
uint32_t memwatch_connects(void) { return s_connects; }

// True when `min` is a new low worth a line: under the threshold and at least
// one step below the last reported value. Updates *logged when it fires.
static bool new_low(uint32_t min, uint32_t *logged) {
    if (min >= MW_WARN_BELOW) return false;
    if (*logged != UINT32_MAX && min + MW_STEP > *logged) return false;
    *logged = min;
    return true;
}

void memwatch_tick(void) {
    // POOL = what malloc() can use (MALLOC_CAP_DEFAULT internal heaps, the
    // figure the "[mem] imin=" line has always shown). ALL = every internal
    // heap, including the 32 KB SPIRAM_MALLOC_RESERVE_INTERNAL region that
    // only explicit internal/DMA allocations (Wi-Fi driver, task stacks) can
    // reach. POOL near zero is survivable (malloc falls back to PSRAM); ALL
    // near zero is where the Wi-Fi driver starts dropping frames.
    uint32_t pool_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT);
    uint32_t all_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    bool lo_pool = new_low(pool_min, &s_pool_logged);
    bool lo_all  = new_low(all_min, &s_all_logged);
    if (lo_pool || lo_all) {
        logln("[mem] LOW internal: pool min %lu (now %lu, blk %lu), all min %lu (now %lu), "
              "connects %lu, ap clients %d",
              (unsigned long)pool_min,
              (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT),
              (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT),
              (unsigned long)all_min,
              (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
              (unsigned long)s_connects, netcore_ap_client_count());
    }

    uint32_t n = s_fail_n;
    if (n != s_fail_logged) {
        logln("[mem] ALLOC FAILED x%lu (total %lu): last %lu B caps 0x%lx in %s",
              (unsigned long)(n - s_fail_logged), (unsigned long)n,
              (unsigned long)s_fail_size, (unsigned long)s_fail_caps,
              s_fail_fn ? s_fail_fn : "?");
        s_fail_logged = n;
    }
}
