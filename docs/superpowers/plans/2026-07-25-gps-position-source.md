# Wired GNSS Position Source — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a wired GNSS receiver as a second, user-selectable position source alongside the Wi-Fi position feed, with automatic fallback, per-field manual identity, a status-coloured display icon, a hardware button, and portal controls that vanish entirely on boards with no GNSS.

**Architecture:** `nmea.c` is a pure parser (host-testable). `gps.c` owns UART1 + the PPS input and publishes a `gps_state_t`. `poller_task` remains the **sole writer** to `pos_state_t`: it becomes an arbiter that picks a source each second and composes the result, substituting manual identity for fields the feed left empty. `flightview.c` derives the display indicator; layouts only place and colour glyphs.

**Tech Stack:** ESP-IDF v5.3.5, C11, FreeRTOS, LVGL (Board 3 only), NVS, plain `clang` for host unit tests.

**Spec:** `docs/superpowers/specs/2026-07-25-gps-position-source-design.md`

## Global Constraints

- Branch: `feat/gps-position-source`. Commit after every task.
- **Pins are never hard-coded.** They come from `board_t`. On the classic ESP32-WROVER GPIO16/17 are the PSRAM lines; driving them blindly is destructive.
- **`poller_task` stays the only caller of `pos_set()`** outside `sim_step()`. `gps.c` never writes `pos_state_t`.
- Boards with `gps_rx == -1` must be **bit-for-bit unchanged**: no task, no UART, no NVS reads, no portal section, no `gps` object in `/status`.
- `nmea.c` and every `*_util`-style pure helper must compile with plain `clang -Imain` — **no ESP-IDF headers, no FreeRTOS types**.
- Host tests are plain clang one-liners, matching `firmware-idf/README.md:137-146`.
- Build: `idf.py -B build -DSDKCONFIG=build/sdkconfig build` from `firmware-idf/`.
- Fleet-safe pins are **GPIO16 (RX) and GPIO12 (TX) only**. PPS on GPIO21 is Board-3-only.
- Existing behaviour is the default: `gps_pref` defaults to feed (`0`).

---

### Task 1: NMEA parser (pure, host-tested)

**Files:**
- Create: `firmware-idf/main/nmea.h`, `firmware-idf/main/nmea.c`
- Test: `firmware-idf/host_test/test_nmea.c`

**Interfaces:**
- Consumes: nothing.
- Produces: `nmea_state_t`, `nmea_reset(nmea_state_t*)`, `nmea_line(nmea_state_t*, const char *line)` returning `bool` (true = sentence accepted), and `nmea_checksum_ok(const char *line)`.

- [ ] **Step 1: Write the failing test**

```c
// firmware-idf/host_test/test_nmea.c
#include "nmea.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define NEAR(a,b) (fabs((a)-(b)) < 1e-6)

int main(void) {
    nmea_state_t s; nmea_reset(&s);

    // --- checksum gate -----------------------------------------------------
    assert(nmea_checksum_ok("$GNGGA,,,,,,0,00,99.99,,,,,,*56"));
    assert(!nmea_checksum_ok("$GNGGA,,,,,,0,00,99.99,,,,,,*00"));
    assert(!nmea_checksum_ok("garbage"));
    assert(!nmea_checksum_ok(""));

    // --- real no-fix GGA captured from the module 2026-07-25 ---------------
    assert(nmea_line(&s, "$GNGGA,,,,,,0,00,99.99,,,,,,*56"));
    assert(s.fix == NMEA_FIX_NONE);
    assert(s.sats_used == 0);
    assert(!s.have_pos);

    // --- real 3D-fix GGA captured from the module 2026-07-25 ---------------
    nmea_reset(&s);
    assert(nmea_line(&s,
        "$GNGGA,012952.00,2217.59913,S,16626.35477,E,1,06,1.30,22.3,M,56.6,M,,*66"));
    assert(s.have_pos);
    assert(s.sats_used == 6);
    assert(NEAR(s.hdop, 1.30));
    assert(NEAR(s.alt_m, 22.3));
    // 22 deg 17.59913 min South -> negative
    assert(s.lat < -22.2933 && s.lat > -22.2934);
    assert(s.lon > 166.4392 && s.lon < 166.4393);

    // --- GSA gives the real fix dimension ----------------------------------
    assert(nmea_line(&s, "$GNGSA,A,3,13,41,08,23,33,,,,,,,,3.36,1.30,3.10,4*0C"));
    assert(s.fix == NMEA_FIX_3D);
    nmea_reset(&s);
    assert(nmea_line(&s, "$GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,4*36"));
    assert(s.fix == NMEA_FIX_NONE);

    // --- RMC: validity, speed, track, date ---------------------------------
    nmea_reset(&s);
    assert(nmea_line(&s,
        "$GNRMC,012656.00,A,2217.59636,S,16626.35420,E,0.724,79.79,250726,,,A,V*28"));
    assert(s.rmc_valid);
    assert(NEAR(s.gs_kt, 0.724));
    assert(NEAR(s.track_deg, 79.79));
    assert(s.utc_ms > 0);
    // void RMC must not claim validity
    nmea_reset(&s);
    assert(nmea_line(&s, "$GNRMC,,V,,,,,,,,,,N,V*37"));
    assert(!s.rmc_valid);

    // --- GSV per-constellation counts, multi-talker ------------------------
    nmea_reset(&s);
    assert(nmea_line(&s, "$GPGSV,1,1,04,01,50,308,,03,23,285,,04,61,344,,26,09,066,,0*XX"
                         ));  // count field is what matters; checksum checked separately
    nmea_reset(&s);
    nmea_line(&s, "$GBGSV,3,1,09,07,41,213,08,08,29,310,21,10,31,216,09,13,16,328,18,1*7D");
    assert(s.sats_bds == 9);
    nmea_line(&s, "$GPGSV,1,1,02,07,,,27,30,,,22,1*6A");
    assert(s.sats_gps == 2);
    assert(s.sats_view == 11);   // 9 BDS + 2 GPS

    // --- robustness ---------------------------------------------------------
    nmea_reset(&s);
    assert(!nmea_line(&s, ""));
    assert(!nmea_line(&s, "$"));
    assert(!nmea_line(&s, "$GNZZZ,1,2,3*4A"));       // unknown sentence, no crash
    assert(!nmea_line(&s, "$GNGGA,012952.00,2217"));  // truncated

    printf("test_nmea OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd firmware-idf && clang -Imain -o /tmp/t host_test/test_nmea.c main/nmea.c -lm && /tmp/t
```
Expected: FAIL — `fatal error: 'nmea.h' file not found`.

- [ ] **Step 3: Write `nmea.h`**

```c
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Pure NMEA 0183 parser. No ESP-IDF types: builds and unit-tests on the host
// with plain clang, exactly like geo.c / derive.c / config_util.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { NMEA_FIX_NONE = 1, NMEA_FIX_2D = 2, NMEA_FIX_3D = 3 } nmea_fix_t;

typedef struct {
    nmea_fix_t fix;          // from GSA field 2
    bool   have_pos;         // GGA carried a usable lat/lon
    bool   rmc_valid;        // RMC status == 'A'
    double lat, lon;         // degrees, N/E positive
    double alt_m;            // GGA altitude, metres MSL
    double hdop;
    double gs_kt, track_deg; // from RMC
    uint64_t utc_ms;         // epoch ms from RMC date+time, 0 when unknown
    int sats_used;           // GGA field 7
    int sats_view;           // sum of per-talker GSV counts
    int sats_gps, sats_glo, sats_gal, sats_bds, sats_qzss;
} nmea_state_t;

// Zero the state (call once at start, and to drop stale counts).
void nmea_reset(nmea_state_t *s);

// True when the "$...*HH" checksum matches. Rejects malformed input.
bool nmea_checksum_ok(const char *line);

// Feed one complete sentence (no CR/LF). Returns true when it was a sentence
// this parser understands AND the checksum passed. Does not validate checksum
// when the line carries none — callers gate on nmea_checksum_ok() first.
bool nmea_line(nmea_state_t *s, const char *line);
```

- [ ] **Step 4: Write `nmea.c`**

Implement with a small comma-field splitter over a bounded local copy (max 120 chars; longer lines are rejected). Key details:

- `nmea_checksum_ok`: XOR every byte between `$` and `*`, compare against the two hex digits. Reject if no `$`, no `*`, or fewer than 2 hex digits.
- Latitude `ddmm.mmmmm` → `dd + mm.mmmmm/60`, negated when hemisphere is `S`. Longitude is `dddmm.mmmmm`, negated on `W`.
- Dispatch on the **last three characters** of the talker+type field (`GGA`, `RMC`, `GSA`, `GSV`) so `GN`/`GP`/`GA`/`GB`/`GQ`/`GL` prefixes all work.
- GSV: field 3 is the in-view count for **that talker**. Store per-constellation by prefix (`GP`→gps, `GL`→glo, `GA`→gal, `GB`→bds, `GQ`→qzss) and recompute `sats_view` as their sum, so repeated GSVs replace rather than accumulate.
- RMC date is `ddmmyy` with a 2000-based century; convert with a plain civil-days algorithm (no `timegm`, which is absent on some hosts).
- Empty fields must leave the corresponding state untouched, never write 0.

- [ ] **Step 5: Run test to verify it passes**

```bash
cd firmware-idf && clang -Imain -o /tmp/t host_test/test_nmea.c main/nmea.c -lm && /tmp/t
```
Expected: `test_nmea OK`

- [ ] **Step 6: Add the test to the README list**

Append to the host-test block in `firmware-idf/README.md` (after line 146):
```
clang -Imain -o /tmp/t host_test/test_nmea.c            main/nmea.c       -lm && /tmp/t
```

- [ ] **Step 7: Commit**

```bash
git add firmware-idf/main/nmea.c firmware-idf/main/nmea.h \
        firmware-idf/host_test/test_nmea.c firmware-idf/README.md
git commit -m "feat(gps): pure NMEA 0183 parser with host tests from real capture"
```

---

### Task 2: Board profile pins + GPS driver

**Files:**
- Modify: `firmware-idf/main/board.h` (add fields to `board_t`), `firmware-idf/main/board.c:23-45` (populate profiles)
- Create: `firmware-idf/main/gps.h`, `firmware-idf/main/gps.c`
- Modify: `firmware-idf/main/CMakeLists.txt` (add `nmea.c`, `gps.c` to SRCS)
- Modify: `firmware-idf/main/aidlink_main.c` (call `gps_start()`)

**Interfaces:**
- Consumes: `nmea_state_t`, `nmea_line()`, `nmea_checksum_ok()` from Task 1; `board_get()`.
- Produces: `gps_state_t`, `void gps_start(void)`, `void gps_get(gps_state_t *out)`, `bool gps_has_hw(void)`.

- [ ] **Step 1: Extend `board_t`**

In `board.h`, add to the struct (with a comment that `-1` means absent):
```c
    int gps_rx, gps_tx, gps_pps;   // UART1 + timepulse; -1 = this model has none
    int btn_gpio;                  // user key; -1 = none
```

- [ ] **Step 2: Populate the profiles**

In `board.c`, add to **`PROF_TDISPLAY_S3` only**:
```c
    // GNSS: RX/TX are the only pins free on every board in the fleet; PPS on 21
    // is Board-3-only (on the T3-S3 it is QWIIC SCL *and* LoRa DIO3).
    .gps_rx = 16, .gps_tx = 12, .gps_pps = 21,
    .btn_gpio = 14,                       // user key, unused by the rest of the firmware
```
And add to `PROF_S3_DEVKIT`, `PROF_T3S3` and `GENERIC`:
```c
    .gps_rx = -1, .gps_tx = -1, .gps_pps = -1, .btn_gpio = -1,
```

- [ ] **Step 3: Write `gps.h`**

```c
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "nmea.h"

typedef struct {
    bool       present;         // latched on first checksum-valid sentence
    nmea_fix_t fix;
    int        sats_used, sats_view;
    int        sats_gps, sats_glo, sats_gal, sats_bds, sats_qzss;
    double     hdop, lat, lon, alt_ft, gs_kt, track_deg;
    uint64_t   utc_ms;
    uint32_t   last_fix_ms;     // monotonic ms of the last positional fix
    uint32_t   last_rx_ms;      // monotonic ms of any valid sentence
    uint32_t   pps_edges, pps_interval_us;
    uint32_t   csum_errors;
} gps_state_t;

// True when this board declares GNSS pins. Everything else is a no-op if false.
bool gps_has_hw(void);

// Start the receiver task. Safe to call on boards without GNSS (does nothing).
void gps_start(void);

// Mutex-guarded snapshot.
void gps_get(gps_state_t *out);
```

- [ ] **Step 4: Write `gps.c`**

- `gps_has_hw()` → `board_get()->gps_rx >= 0`.
- `gps_start()` returns immediately when `!gps_has_hw()`.
- Install UART1 at **9600 8N1**, RX = `board->gps_rx`, TX = `board->gps_tx`, 2048-byte RX buffer.
- Configure `gps_pps` as input with `GPIO_INTR_POSEDGE`, install the ISR service, add the handler, **then call `gpio_intr_enable()`** — `gpio_config()` with `GPIO_INTR_DISABLE` followed by `gpio_set_intr_type()` leaves the interrupt off and makes a good wire look dead (see `LEARNING.md` 2026-07-25).
- Task loop: `uart_read_bytes` with a 200 ms timeout, accumulate into a 128-byte line buffer, split on `\r`/`\n`, discard over-long lines. For each line: `nmea_checksum_ok()` → on failure bump `csum_errors` and drop; on success `nmea_line()`, then copy into the guarded `gps_state_t`, set `last_rx_ms`, set `present = true`, and set `last_fix_ms` only when `have_pos && rmc_valid`.
- Convert altitude to feet (`alt_m * 3.280839895`) when storing `alt_ft`.
- Clear `present` after **60 s** with no valid sentence, and zero the satellite counts so a stale display cannot show phantom satellites.
- Guard the whole file body with nothing — it must compile on every target; the runtime `gps_has_hw()` check is what disables it.

- [ ] **Step 5: Wire into the build and boot**

Add `"nmea.c"` and `"gps.c"` to `SRCS` in `main/CMakeLists.txt`. In `aidlink_main.c`, call `gps_start();` immediately before `poller_start(&cfg);`.

- [ ] **Step 6: Build**

```bash
cd firmware-idf && source ~/esp/esp-idf/export.sh && idf.py -B build -DSDKCONFIG=build/sdkconfig build
```
Expected: builds clean.

- [ ] **Step 7: Commit**

```bash
git add firmware-idf/main/board.h firmware-idf/main/board.c firmware-idf/main/gps.c \
        firmware-idf/main/gps.h firmware-idf/main/CMakeLists.txt firmware-idf/main/aidlink_main.c
git commit -m "feat(gps): UART1 receiver task, PPS counter, per-board pins"
```

---

### Task 3: Config keys — GPS switches and manual identity

**Files:**
- Modify: `firmware-idf/main/config.h:49-57`, `firmware-idf/main/config.c:63` (defaults), `:108` (load), `:159` (save)

**Interfaces:**
- Produces: `cfg.gps_enable`, `cfg.gps_pref`, `cfg.id_tail`, `cfg.id_flight`, `cfg.id_orig`, `cfg.id_dest`.

- [ ] **Step 1: Add fields to `config.h`**

```c
    // --- GNSS position source ---
    bool     gps_enable;       // allow GPS to be selected at all
    int      gps_pref;         // preferred source: 0 = Wi-Fi feed, 1 = GPS
    // --- manual aircraft identity (persisted) ---
    // Used ONLY for fields the live feed leaves empty; the feed always wins.
    // Distinct from ac_tail/ac_type above, which stay RAM-only and feed-filled.
    char     id_tail[12], id_flight[16], id_orig[8], id_dest[8];
```

Also update the comment block at `config.h:46-49` so it no longer claims identity is "never user-set" — the feed stays authoritative, but is no longer the only source.

- [ ] **Step 2: Defaults, load, save**

`config.c` near line 63:
```c
    c->gps_enable = true; c->gps_pref = 0;
    c->id_tail[0] = c->id_flight[0] = c->id_orig[0] = c->id_dest[0] = 0;
```
Near line 108:
```c
    c->gps_enable = get_bool(h, "gps_en", c->gps_enable);
    c->gps_pref   = get_i32 (h, "gps_pref", c->gps_pref);
    get_str(h, "id_tail",   c->id_tail,   sizeof c->id_tail);
    get_str(h, "id_flight", c->id_flight, sizeof c->id_flight);
    get_str(h, "id_orig",   c->id_orig,   sizeof c->id_orig);
    get_str(h, "id_dest",   c->id_dest,   sizeof c->id_dest);
```
Near line 159:
```c
    e |= nvs_set_u8 (h, "gps_en",   c->gps_enable ? 1 : 0);
    e |= nvs_set_i32(h, "gps_pref", c->gps_pref);
    e |= nvs_set_str(h, "id_tail",   c->id_tail);
    e |= nvs_set_str(h, "id_flight", c->id_flight);
    e |= nvs_set_str(h, "id_orig",   c->id_orig);
    e |= nvs_set_str(h, "id_dest",   c->id_dest);
```
Use whatever int getter `config.c` already defines (`get_i32`); if none exists, add one mirroring `get_bool`.

- [ ] **Step 3: Build and commit**

```bash
cd firmware-idf && idf.py -B build -DSDKCONFIG=build/sdkconfig build
git add firmware-idf/main/config.c firmware-idf/main/config.h
git commit -m "feat(gps): persist GPS source prefs and manual identity fallback"
```

---

### Task 4: Arbitration + identity precedence

**Files:**
- Create: `firmware-idf/main/possrc.h`, `firmware-idf/main/possrc.c` (pure choice + identity helpers)
- Test: `firmware-idf/host_test/test_possrc.c`
- Modify: `firmware-idf/main/poller.c:43-92` (`apply_fix` caches instead of writing), `:243-258` (`poller_task` arbitrates)

**Interfaces:**
- Consumes: `gps_get()`, `gps_state_t`, `cfg.gps_*`, `cfg.id_*`.
- Produces: `possrc_t`, `possrc_choose(bool,bool,int)`, `possrc_ident(char *dst, size_t cap, const char *feed, const char *manual)`.

- [ ] **Step 1: Write the failing test**

```c
// firmware-idf/host_test/test_possrc.c
#include "possrc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    // pref = GPS (1)
    assert(possrc_choose(true,  true,  1) == SRC_GPS);
    assert(possrc_choose(true,  false, 1) == SRC_GPS);
    assert(possrc_choose(false, true,  1) == SRC_FEED);
    assert(possrc_choose(false, false, 1) == SRC_NONE);
    // pref = FEED (0)
    assert(possrc_choose(true,  true,  0) == SRC_FEED);
    assert(possrc_choose(false, true,  0) == SRC_FEED);
    assert(possrc_choose(true,  false, 0) == SRC_GPS);
    assert(possrc_choose(false, false, 0) == SRC_NONE);

    // identity precedence: feed wins whenever non-empty
    char d[12];
    possrc_ident(d, sizeof d, "F-ONET", "F-XXXX"); assert(!strcmp(d, "F-ONET"));
    possrc_ident(d, sizeof d, "",       "F-XXXX"); assert(!strcmp(d, "F-XXXX"));
    possrc_ident(d, sizeof d, "",       "");       assert(d[0] == 0);
    // manual longer than the field is truncated, never overflows
    possrc_ident(d, sizeof d, "", "ABCDEFGHIJKLMNOP"); assert(strlen(d) == sizeof d - 1);

    printf("test_possrc OK\n");
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd firmware-idf && clang -Imain -o /tmp/t host_test/test_possrc.c main/possrc.c && /tmp/t
```
Expected: FAIL — `'possrc.h' file not found`.

- [ ] **Step 3: Implement `possrc.h` / `possrc.c`**

```c
// possrc.h
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum { SRC_NONE, SRC_FEED, SRC_GPS } possrc_t;

// Preferred source wins when live; otherwise the other one; otherwise nothing.
// pref: 0 = feed, 1 = GPS.
possrc_t possrc_choose(bool gps_live, bool feed_live, int pref);

// Per-field identity precedence: the feed value wins whenever it is non-empty,
// the manual value fills only what the feed left blank. Always NUL-terminates.
void possrc_ident(char *dst, size_t cap, const char *feed, const char *manual);
```

`possrc.c` is a direct transcription — `possrc_choose` is two ifs; `possrc_ident` copies `feed[0] ? feed : manual` with a bounded copy.

- [ ] **Step 4: Run to verify it passes**

```bash
cd firmware-idf && clang -Imain -o /tmp/t host_test/test_possrc.c main/possrc.c && /tmp/t
```
Expected: `test_possrc OK`

- [ ] **Step 5: Refactor `apply_fix` to cache instead of publish**

In `poller.c`, add a file-scope cache and stop calling `pos_set()` from `apply_fix`:
```c
typedef struct {
    bool   valid;
    double lat, lon, alt_ft, gs_kt, track_deg;
    bool   have_track;
    uint64_t utc_ms;
    uint32_t at_ms;
    char   flight[16], tail[12], orig[8], dest[8];
} feedfix_t;
static feedfix_t s_feed;
```
`apply_fix` fills `s_feed` (keeping the existing `derive_update` call and the identity/`perf_type` side effects at lines 66-91 exactly as they are) and returns. It must no longer call `pos_set`.

- [ ] **Step 6: Add the arbiter**

New static function in `poller.c`, called once per second from `poller_task`:
```c
static void arbitrate(void) {
    uint32_t t = now_ms();
    gps_state_t g; gps_get(&g);

    bool gps_live  = CFG->gps_enable && g.present && g.fix != NMEA_FIX_NONE &&
                     (t - g.last_fix_ms) < 5000;
    bool feed_live = s_feed.valid && (t - s_feed.at_ms) < CFG->stale_ms;

    possrc_t src = possrc_choose(gps_live, feed_live, CFG->gps_pref);
    if (src == SRC_NONE) { pos_mark_stale(); s_live_src = SRC_NONE; return; }

    pos_state_t p; pos_get(&p);
    p.valid = true; p.simulated = false; p.fixed = false; p.service_avail = true;
    if (src == SRC_GPS) {
        p.lat = g.lat; p.lon = g.lon; p.alt_ft = g.alt_ft;
        p.gs_kt = g.gs_kt; p.track_deg = g.track_deg; p.have_track = true;
        if (g.utc_ms) p.utc_ms = g.utc_ms;
        p.last_fix_ms = g.last_fix_ms;
    } else {
        p.lat = s_feed.lat; p.lon = s_feed.lon; p.alt_ft = s_feed.alt_ft;
        p.gs_kt = s_feed.gs_kt; p.track_deg = s_feed.track_deg;
        p.have_track = s_feed.have_track;
        if (s_feed.utc_ms) p.utc_ms = s_feed.utc_ms;
        p.last_fix_ms = s_feed.at_ms;
    }
    // Identity is decoupled from position: always the feed's, with the manual
    // fallback filling only what the feed left empty.
    possrc_ident(p.flight, sizeof p.flight, s_feed.flight, CFG->id_flight);
    possrc_ident(p.tail,   sizeof p.tail,   s_feed.tail,   CFG->id_tail);
    possrc_ident(p.orig,   sizeof p.orig,   s_feed.orig,   CFG->id_orig);
    possrc_ident(p.dest,   sizeof p.dest,   s_feed.dest,   CFG->id_dest);
    pos_set(&p);
    s_live_src = src;
}
```
Add `static possrc_t s_live_src;` and expose `possrc_t poller_live_source(void)` in `poller.h` for the display and `/status`.

In `poller_task`, replace the stale-watchdog block with a call to `arbitrate()` (keep the `sim_enable` branch untouched).

- [ ] **Step 7: Build, then commit**

```bash
cd firmware-idf && idf.py -B build -DSDKCONFIG=build/sdkconfig build
git add firmware-idf/main/possrc.c firmware-idf/main/possrc.h firmware-idf/main/poller.c \
        firmware-idf/main/poller.h firmware-idf/host_test/test_possrc.c firmware-idf/README.md
git commit -m "feat(gps): source arbitration with fallback and per-field identity precedence"
```

---

### Task 5: Display indicator — satellite glyph and quality colour

**Files:**
- Create: `tools/gen_sat_font.py`, `firmware-idf/main/font_sat.c`
- Modify: `firmware-idf/main/flightview.h:63-71`, `firmware-idf/main/flightview.c:246-252`
- Modify: `firmware-idf/main/layout_tdisplay.c:32-49` (palette + handle), `:129-132` (build), `:359-361` (status)
- Modify: `firmware-idf/main/layout_oled.c` (glyph swap only)
- Test: `firmware-idf/host_test/test_gpsqual.c`

**Interfaces:**
- Consumes: `poller_live_source()`, `gps_get()`.
- Produces: `fv_src_t`, `fv_gq_t`, `fv_gps_quality(nmea_fix_t, double hdop, int sats_used, int sats_view, bool silent)`.

- [ ] **Step 1: Write the failing quality-mapping test**

```c
// firmware-idf/host_test/test_gpsqual.c
#include "gpsqual.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    // green: 3D and HDOP <= 2.0 and sats >= 5
    assert(fv_gps_quality(NMEA_FIX_3D, 1.30, 6, 12, false) == FV_GQ_GREEN);
    assert(fv_gps_quality(NMEA_FIX_3D, 2.00, 5, 9,  false) == FV_GQ_GREEN);
    // orange: 2D, or 3D with bad HDOP, or 3D with too few sats
    assert(fv_gps_quality(NMEA_FIX_2D, 1.00, 8, 9,  false) == FV_GQ_ORANGE);
    assert(fv_gps_quality(NMEA_FIX_3D, 2.01, 8, 9,  false) == FV_GQ_ORANGE);
    assert(fv_gps_quality(NMEA_FIX_3D, 1.00, 4, 9,  false) == FV_GQ_ORANGE);
    // red: no fix but something in view
    assert(fv_gps_quality(NMEA_FIX_NONE, 99.99, 0, 1, false) == FV_GQ_RED);
    // purple: nothing in view at all
    assert(fv_gps_quality(NMEA_FIX_NONE, 99.99, 0, 0, false) == FV_GQ_PURPLE);
    // grey: present but gone quiet, regardless of the last known numbers
    assert(fv_gps_quality(NMEA_FIX_3D, 1.00, 8, 9, true) == FV_GQ_GREY);

    printf("test_gpsqual OK\n");
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd firmware-idf && clang -Imain -o /tmp/t host_test/test_gpsqual.c main/gpsqual.c && /tmp/t
```
Expected: FAIL — `'gpsqual.h' file not found`.

- [ ] **Step 3: Implement `gpsqual.h` / `gpsqual.c`**

Pure, host-testable, includes only `nmea.h`:
```c
typedef enum { FV_GQ_NONE, FV_GQ_PURPLE, FV_GQ_RED, FV_GQ_ORANGE,
               FV_GQ_GREEN, FV_GQ_GREY } fv_gq_t;

fv_gq_t fv_gps_quality(nmea_fix_t fix, double hdop, int sats_used,
                       int sats_view, bool silent);
```
Order of tests: `silent` → GREY; `fix == NONE` → `sats_view ? RED : PURPLE`; `fix == 3D && hdop <= 2.0 && sats_used >= 5` → GREEN; else ORANGE.

- [ ] **Step 4: Run to verify it passes**

```bash
cd firmware-idf && clang -Imain -o /tmp/t host_test/test_gpsqual.c main/gpsqual.c && /tmp/t
```
Expected: `test_gpsqual OK`

- [ ] **Step 5: Extend `fv_status_t` and populate it**

In `flightview.h` add `typedef enum { FV_SRC_FEED, FV_SRC_GPS } fv_src_t;` and two fields to `fv_status_t`:
```c
    fv_src_t source;      // which source is ACTUALLY live right now
    fv_gq_t  gps_quality; // meaningful only when source == FV_SRC_GPS
```
In `flightview_status()` (after the existing `feed_active` block at `flightview.c:248-251`):
```c
    s->source = (poller_live_source() == SRC_GPS) ? FV_SRC_GPS : FV_SRC_FEED;
    s->gps_quality = FV_GQ_NONE;
    if (gps_has_hw()) {
        gps_state_t g; gps_get(&g);
        if (g.present) {
            bool silent = (now - g.last_rx_ms) > 5000;
            s->gps_quality = fv_gps_quality(g.fix, g.hdop, g.sats_used,
                                            g.sats_view, silent);
        }
    }
```

- [ ] **Step 6: Generate the satellite glyph**

Copy `tools/gen_cloud_font.py` to `tools/gen_sat_font.py`, changing the codepoint to **U+1F6F0** (satellite) or, if the source font lacks it, **U+25C9** rendered as a dish. Emit `firmware-idf/main/font_sat.c` declaring `font_sat`. Add `LV_FONT_DECLARE(font_sat);` beside the others at the top of `layout_tdisplay.c` and add `font_sat.c` to `SRCS`.

- [ ] **Step 7: Apply glyph and colour in the layout**

Add to the palette block (`layout_tdisplay.c:32-42`):
```c
#define COL_PURPLE  0xA855F7   // GPS with zero satellites — deliberately NOT
                               // COL_MAGENTA, which means "Wi-Fi feed"
```
Add `static lv_obj_t *s_sat;` beside `s_feed`. In `build()`, create `s_sat` at the same coordinates as `s_feed` (186, 142) with `font_sat`, and hide it initially.

Replace the status update at `:359-361`:
```c
    lv_obj_set_style_text_color(s_globe,
        lv_color_hex(s->internet ? COL_INET : COL_RED), 0);

    bool gps = (s->source == FV_SRC_GPS);
    lv_obj_add_flag(gps ? s_feed : s_sat, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(gps ? s_sat : s_feed, LV_OBJ_FLAG_HIDDEN);
    if (gps) {
        uint32_t c = COL_DIMMED;
        switch (s->gps_quality) {
            case FV_GQ_GREEN:  c = COL_GREEN;  break;
            case FV_GQ_ORANGE: c = COL_AMBER;  break;
            case FV_GQ_RED:    c = COL_RED;    break;
            case FV_GQ_PURPLE: c = COL_PURPLE; break;
            default:           c = COL_DIMMED; break;
        }
        lv_obj_set_style_text_color(s_sat, lv_color_hex(c), 0);
    } else {
        lv_obj_set_style_text_color(s_feed,
            lv_color_hex(s->feed_active ? COL_MAGENTA : COL_DIMMED), 0);
    }
```

- [ ] **Step 8: OLED parity**

In `layout_oled.c`, swap the feed marker character when `s->source == FV_SRC_GPS`. Mono panel: no colour, glyph only.

- [ ] **Step 9: Build and commit**

```bash
cd firmware-idf && idf.py -B build -DSDKCONFIG=build/sdkconfig build
git add tools/gen_sat_font.py firmware-idf/main/font_sat.c firmware-idf/main/gpsqual.c \
        firmware-idf/main/gpsqual.h firmware-idf/main/flightview.c firmware-idf/main/flightview.h \
        firmware-idf/main/layout_tdisplay.c firmware-idf/main/layout_oled.c \
        firmware-idf/main/CMakeLists.txt firmware-idf/host_test/test_gpsqual.c firmware-idf/README.md
git commit -m "feat(gps): satellite indicator coloured by fix quality"
```

---

### Task 6: User-key button — hold toggles source, tap shows detail

**Files:**
- Create: `firmware-idf/main/button.h`, `firmware-idf/main/button.c`
- Modify: `firmware-idf/main/aidlink_main.c` (start it), `firmware-idf/main/layout_tdisplay.c` (overlay), `firmware-idf/main/flightview.h` (overlay fields)

**Interfaces:**
- Consumes: `board_get()->btn_gpio`, `gps_get()`, `cfg`.
- Produces: `void button_start(aidlink_cfg_t *cfg);`

- [ ] **Step 1: Write `button.c`**

Task polling `btn_gpio` every 20 ms with a 30 ms debounce (active-low, internal pull-up). On release: press duration `< 1000 ms` → short-tap callback; `>= 1000 ms` → long-press callback (fire on **threshold crossing**, not on release, so the user gets feedback while still holding).

Both gestures return immediately when `!gps_has_hw()` or `!gps_present` — there is nothing to toggle or show.

Long press: `cfg->gps_pref = !cfg->gps_pref; cfg_save(cfg);` then set an overlay request of kind "source", expiring in 2000 ms.
Short tap: set an overlay request of kind "detail", expiring in 4000 ms.

Expose the overlay request through two fields added to `fv_status_t`:
```c
    int  overlay;        // 0 = none, 1 = source banner, 2 = GPS detail
    char overlay_l1[24], overlay_l2[24];
```
`flightview_status()` fills the text when an overlay is active:
- source banner: `"SOURCE: GPS"` / `"SOURCE: FEED"`
- detail line 1: `"3D  6/12 sats  HDOP 1.3"`
- detail line 2: `"GPS 2 BDS 4  PPS ok"` (PPS ok when `pps_edges` advanced within 2 s)

- [ ] **Step 2: Render the overlay**

In `layout_tdisplay.c` `build()`, create a hidden centred two-line label group. In `status()`, show it when `s->overlay != 0` with the supplied strings, hide otherwise.

- [ ] **Step 3: Start it**

In `aidlink_main.c`, `button_start(&cfg);` after `gps_start();`. `button_start` returns immediately when `board_get()->btn_gpio < 0`.

- [ ] **Step 4: Build and commit**

```bash
cd firmware-idf && idf.py -B build -DSDKCONFIG=build/sdkconfig build
git add firmware-idf/main/button.c firmware-idf/main/button.h firmware-idf/main/aidlink_main.c \
        firmware-idf/main/layout_tdisplay.c firmware-idf/main/flightview.c \
        firmware-idf/main/flightview.h firmware-idf/main/CMakeLists.txt
git commit -m "feat(gps): user-key gestures — hold toggles source, tap shows GPS detail"
```

---

### Task 7: Portal — GPS section, identity fallback, /status

**Files:**
- Modify: `firmware-idf/main/web.c` (settings render ~`:383-414`, POST parse ~`:626-629`, `/status` JSON)

- [ ] **Step 1: Identity fallback section (all boards)**

Rendered unconditionally, since a feed that omits route benefits too:
```c
    sect(r, "Aircraft identity (fallback)");
    ff_text(r, "Tail",        "idTail",   c->id_tail,   "text", false);
    ff_text(r, "Flight",      "idFlight", c->id_flight, "text", false);
    ff_text(r, "Origin",      "idOrig",   c->id_orig,   "text", false);
    ff_text(r, "Destination", "idDest",   c->id_dest,   "text", false);
```
Beside each, print the live effective value and whether it came from the feed or config, read from `pos_get()` and `cfg`.

- [ ] **Step 2: GPS section (only when detected)**

```c
    if (gps_has_hw()) {
        gps_state_t g; gps_get(&g);
        if (g.present) {
            sect(r, "GNSS");
            ff_tog(r, "Enable GPS source", "gpsEn", c->gps_enable);
            ff_tog(r, "Prefer GPS over Wi-Fi feed", "gpsPref", c->gps_pref == 1);
            // read-only live detail: fix, sats used/view, HDOP, per-constellation,
            // PPS interval, checksum errors
        }
    }
```
When `gps_has_hw()` but `!g.present`, render nothing at all — per spec, detection decides visibility.

- [ ] **Step 3: Parse the POST**

Mirror the `perf_type` pattern near `:626`:
```c
    if (fld(body, "idTail",   v, sizeof v)) { upper_trim(v); strlcpy(c->id_tail,   v, sizeof c->id_tail); }
    if (fld(body, "idFlight", v, sizeof v)) { upper_trim(v); strlcpy(c->id_flight, v, sizeof c->id_flight); }
    if (fld(body, "idOrig",   v, sizeof v)) { upper_trim(v); strlcpy(c->id_orig,   v, sizeof c->id_orig); }
    if (fld(body, "idDest",   v, sizeof v)) { upper_trim(v); strlcpy(c->id_dest,   v, sizeof c->id_dest); }
    c->gps_enable = fld(body, "gpsEn", v, sizeof v);
    c->gps_pref   = fld(body, "gpsPref", v, sizeof v) ? 1 : 0;
```
Add a small static `upper_trim()` (uppercase, strip leading/trailing spaces) in `web.c`.

- [ ] **Step 4: `/status` JSON**

Append the `gps` object **only when `gps_has_hw()`**, plus `"live_source":"gps"|"feed"|"none"` from `poller_live_source()`.

- [ ] **Step 5: Build and commit**

```bash
cd firmware-idf && idf.py -B build -DSDKCONFIG=build/sdkconfig build
git add firmware-idf/main/web.c
git commit -m "feat(gps): portal controls for GNSS source and identity fallback"
```

---

### Task 8: On-target verification

- [ ] **Step 1: Restore a known-good baseline first**

The unit currently runs the GPS bench probe, not AIDlink. Reflash from the new build.

- [ ] **Step 2: Flash**

```bash
cd firmware-idf && esptool.py -p /dev/cu.usbmodem101 --before usb_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x10000 build/aidlink.bin
```
Never `erase_flash` — NVS at `0x9000` holds the Wi-Fi credentials.

- [ ] **Step 3: Verify the matrix**

| Check | Expected |
|---|---|
| Boot with GPS attached | portal shows GNSS section; `/status` has `gps` |
| `gps_pref` = GPS, good fix | green satellite glyph; `live_source":"gps"` |
| Unplug antenna 10 s | glyph goes purple, then falls back to feed |
| `gps_pref` = feed | magenta upload glyph returns, unchanged from today |
| Hold user key 1 s | banner `SOURCE: GPS`, choice survives reboot |
| Tap user key | 4 s detail overlay with sats/HDOP/PPS |
| Blank the feed identity, set `id_*` | display shows the manual tail/flight/route |
| Board 2 or 4 (no GPS pins) | no GNSS section, no `gps` in `/status`, no new task |

- [ ] **Step 4: Update docs and commit**

Add a short section to `ESP32-BOARDS.md` under Board 3 recording the GNSS pins and the 5 V requirement, and append the outcome to `LEARNING.md`.

```bash
git add ESP32-BOARDS.md LEARNING.md
git commit -m "docs(gps): record verified GNSS wiring and on-target results"
```

---

## Self-Review

**Spec coverage:** source selection (T4) · GPS on/off switch (T3, T7) · preferred source + fallback (T4) · detection-gated visibility (T2, T7) · identity decoupling (T4) · manual identity fallback (T3, T4, T7) · display glyph + 5 colours (T5) · button hold/tap (T6) · portal detail incl. constellation (T7) · `/status` (T7) · no-GPS boards inert (T2, T7, T8) · host tests (T1, T4, T5). No gaps.

**Placeholder scan:** no TBD/TODO. Every code step carries real code or an exact, decidable instruction.

**Type consistency:** `nmea_fix_t` is defined once in `nmea.h` and reused by `gps.h` and `gpsqual.h` — the enum is deliberately *not* redeclared. `possrc_t` is defined in `possrc.h` and used by `poller.h` and `web.c`. `fv_gq_t`/`fv_src_t` live in `gpsqual.h`/`flightview.h` respectively and are consumed by both layouts. The spec's `gps_state_t` used a local `gps_fix_t`; this plan standardises on `nmea_fix_t` to avoid two enums for one concept.
