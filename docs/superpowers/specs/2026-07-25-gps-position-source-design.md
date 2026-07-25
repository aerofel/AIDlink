# AIDlink — wired GNSS as a selectable position source

**Status:** approved design (2026-07-25)
**Applies to:** `firmware-idf` (all targets). GPS hardware is present on Board 3 only today.

## Goal

Give AIDlink a **second position source**: a wired GNSS receiver on UART, selectable against the
existing Wi-Fi position feed (Viasat/Panasonic/custom poller). The chosen source is a *preference*,
not a lock — whichever source is actually live feeds Jeppesen, so a GNSS dropout or a feed stall
never blanks the EFB.

Boards without a GNSS module must behave **exactly as they do today**: no UART task, no NVS reads,
and no GPS controls anywhere in the portal.

## Hardware context (bench-validated 2026-07-25)

Verified on Board 3 (LilyGO T-Display-S3, MAC `d0:cf:13:32:2f:48`) with a module sold as
"NEO-M8N". See `LEARNING.md` 2026-07-25 for the full session.

| | |
|---|---|
| Receiver | **u-blox M10** silicon (`hwVersion=000A0000`, `PROTVER=34.10`, `ROM SPG 5.10`) under a counterfeit `NEO-M8N-0-10` can |
| Wiring | RX **GPIO16**, TX **GPIO12**, PPS **GPIO21**, VCC **5V**, GND |
| Serial | 9600 8N1, NMEA 0183 |
| PPS | 1.000 s interval, ~100 ms pulse (10 % duty) |
| Antenna | active, **3.0–5.0 V** — 3V3 starves it (AGC pinned at 11 %); 5V gives AGC 30–45 % |

Two constraints follow from the hardware and must be respected by this design:

- **GPIO16 and GPIO12 are the only pins free on every board in the fleet.** On the T3-S3, 17 = OLED
  SCL, 18 = OLED SDA, 21 = QWIIC SCL *and* LoRa DIO3, 10 = QWIIC SDA + LoRa DIO4, 2/11/13/14 =
  microSD, 43/44 = UART0 console. **PPS on GPIO21 is therefore Board-3-only.**
- **On the classic ESP32-WROVER, GPIO16/17 are the PSRAM lines.** Pins must never be chosen by a
  shared `#define`; they come from the per-board profile or the subsystem stays inert.

Configuration note for implementers: this is M10 silicon, so the legacy `UBX-CFG-NAV5` message does
not exist. Any future dynamic-model work must use `UBX-CFG-VALSET`
(`CFG-NAVSPG-DYNMODEL` = `0x20110021`, value 8 = airborne <4g). **Out of scope for this spec** —
the receiver is used in its default NMEA configuration here.

## Architecture

`pos.c` holds one mutex-guarded `pos_state_t` whose invariant is *one writer, never a torn read*.
That invariant is preserved: **`poller_task` remains the sole writer.**

```
  nmea.c  (pure parser, no ESP-IDF types)
     ^
     |
  gps.c   UART1 + PPS ISR  ->  gps_state_t          (produces, no policy)
                                    \
                                     -> poller_task (arbiter, 1 Hz) -> pos_set()
                                    /
  poller_sources.c  HTTP feed  ->  feed fix
```

Alternatives considered and rejected: having `gps.c` write `pos_state_t` directly with source tags
(doubles the writers and pushes policy into a storage module), and a general position-source
registry (YAGNI for two sources).

### Module boundaries

**New**

| File | Responsibility | Depends on |
|---|---|---|
| `nmea.c/h` | Parse GGA/RMC/GSA/GSV into a struct. Checksum validation. **No ESP-IDF types** so it builds and tests on the host. | nothing |
| `gps.c/h` | Own UART1 and the PPS input; feed bytes to `nmea`; maintain `gps_state_t`; latch presence. No source policy. | `nmea`, `board` |
| `button.c/h` | Debounced GPIO with short-press / long-press callbacks. Pin from the board profile. | `board` |
| `tools/gen_sat_font.py`, `font_sat.c` | Single-glyph satellite icon, same pattern as `font_cloud`/`font_globe`. | — |

**Changed**

| File | Change |
|---|---|
| `board.h/c` | `board_t` gains `gps_rx`, `gps_tx`, `gps_pps`, `btn_gpio` (`-1` = absent). Set on `PROF_TDISPLAY_S3` only. |
| `poller.c` | Source arbitration; identity decoupling. |
| `config.h/c` | `gps_enable`, `gps_pref` + NVS load/save. |
| `flightview.h/c` | `fv_status_t` gains `source` and `gps_quality`; overlay state. |
| `layout_tdisplay.c` | Satellite glyph + colour; source/detail overlay. |
| `layout_oled.c` | Glyph swap only (mono panel, no colour). |
| `web.c` | GPS settings section; `/status` fields. |

## Data model

```c
typedef enum { GPS_FIX_NONE = 1, GPS_FIX_2D = 2, GPS_FIX_3D = 3 } gps_fix_t;

typedef struct {
    bool      present;        // latched: a checksum-valid sentence has been seen
    gps_fix_t fix;
    int       sats_used;      // GGA field 7
    int       sats_view;      // summed across GSV talkers
    double    hdop;
    double    lat, lon, alt_m, gs_kt, track_deg;
    uint64_t  utc_ms;
    uint32_t  last_fix_ms;    // monotonic, freshness gate
    uint32_t  last_rx_ms;     // monotonic, any valid sentence
    int       sats_gps, sats_glo, sats_gal, sats_bds, sats_qzss;
    uint32_t  pps_edges;      // total since boot
    uint32_t  pps_interval_us;
    uint32_t  csum_errors;
} gps_state_t;
```

## Arbitration

Runs once per second inside `poller_task`.

1. `cfg.sim_enable` → emulator wins. **Unchanged behaviour.**
2. A source is **live** when its last fix is younger than its stale window:
   GPS **5 s**; feed `cfg.stale_ms`. GPS additionally requires `cfg.gps_enable`,
   `gps.present`, and `fix != GPS_FIX_NONE`.
3. Choose `cfg.gps_pref` if live; else the other source if live; else `pos_mark_stale()`.
4. Compose the written `pos_state_t` as **kinematics from the chosen source + identity always
   carried from the last feed values**.

Step 4 is essential. GNSS supplies no tail, flight number or route, so without decoupling, the
identity row, route, ETA, trip bar and destination-local clock would all blank the moment GPS took
over. `flight`/`tail`/`orig`/`dest` therefore persist across a source switch.

The choice function is **pure and host-tested**:

```c
typedef enum { SRC_NONE, SRC_FEED, SRC_GPS } possrc_t;
possrc_t pos_choose(bool gps_live, bool feed_live, int pref);
```

| `pref` | gps_live | feed_live | result |
|---|---|---|---|
| GPS | yes | any | `SRC_GPS` |
| GPS | no | yes | `SRC_FEED` |
| GPS | no | no | `SRC_NONE` |
| FEED | any | yes | `SRC_FEED` |
| FEED | yes | no | `SRC_GPS` |
| FEED | no | no | `SRC_NONE` |

## Detection and visibility

Pins come from the board profile purely as a **safety gate**; visibility is decided by runtime
detection.

- `board->gps_rx == -1` → GPS task never starts, UART never opened, `gps_enable`/`gps_pref` never
  read, portal renders no GPS section, `/status` omits the `gps` object. This is the
  "ESP32 with no GPS module" case and costs nothing at runtime.
- Otherwise the GPS task runs continuously. `present` latches `true` on the first checksum-valid
  sentence and is what gates the portal section, the display glyph and the button.
- Detection is **continuous, not a one-shot boot window**, so a receiver that starts slowly makes
  the section appear as soon as it talks rather than staying hidden until a reboot.
- `present` does not clear on brief loss (no UI flapping). **60 s** with no valid sentence clears it;
  the section disappears and the arbiter falls back to the feed.

## Display

`flightview.c` derives the indicator; layouts only place and colour glyphs, never decide.

```c
typedef enum { FV_SRC_FEED, FV_SRC_GPS } fv_src_t;
typedef enum { FV_GQ_NONE, FV_GQ_PURPLE, FV_GQ_RED, FV_GQ_ORANGE, FV_GQ_GREEN, FV_GQ_GREY } fv_gq_t;
```

`fv_status_t` gains `fv_src_t source` and `fv_gq_t gps_quality`.

**Feed live** → today's magenta `LV_SYMBOL_UPLOAD`, blipping on `pos_fix_seq()`. Unchanged.

**GPS live** → satellite glyph, coloured:

| Colour | Hex | Condition |
|---|---|---|
| Green | `COL_GREEN` `0x34D399` | 3D fix **and** HDOP ≤ 2.0 **and** `sats_used` ≥ 5 |
| Orange | `COL_AMBER` `0xFFB300` | 2D fix, **or** 3D with HDOP > 2.0, **or** 3D with `sats_used` < 5 |
| Red | `COL_RED` `0xFF3B30` | no fix, `sats_view` ≥ 1 |
| Purple | **new** `COL_PURPLE` `0xA855F7` | `sats_view` == 0 |
| Grey | `COL_DIMMED` `0x3A3A3A` | present but silent > 5 s |

Purple must be a **new** palette entry, not `COL_MAGENTA`: magenta is the feed indicator, and
reusing it would make "GPS with 0 satellites" and "Wi-Fi feed" render in the same colour. The
glyphs differ, but the colour must differ too — the colour is the at-a-glance signal.

Board 4's mono OLED swaps the glyph only; `fv_gq_t` compiles everywhere but is ignored there.

## Button — GPIO14, boards declaring `btn_gpio`

30 ms debounce; 1000 ms long-press threshold. Both gestures are ignored when `!gps.present`.

- **Hold ≥ 1 s** → toggle `cfg.gps_pref`, persist via `cfg_save()`, show `SOURCE: GPS` /
  `SOURCE: FEED` for ~2 s.
- **Short tap** → GPS detail overlay for ~4 s: fix type, `sats_used`/`sats_view`, HDOP,
  per-constellation counts, PPS status.

Long-press-toggles rather than tap-toggles because a stray press silently changes what Jeppesen is
being fed.

## Portal

**Settings** — a GPS section rendered only when `gps.present`:

- `gps_enable` switch — off means GPS is never selected even with a valid fix.
- `gps_pref` — preferred source (feed | GPS).
- Read-only live detail: fix, sats used / in view, HDOP, constellations, PPS, checksum errors.

**`/status` JSON** gains, only when the board has GPS pins:

```json
"gps": { "present": true, "enabled": true, "fix": 3, "sats_used": 6, "sats_view": 12,
         "hdop": 1.30, "gps": 2, "glo": 0, "gal": 0, "bds": 4, "qzss": 0,
         "pps_hz": 1.0, "csum_err": 0, "live_source": "gps" }
```

`live_source` reports what is **actually** feeding position, which may differ from `gps_pref`
during fallback.

## Config

Two new NVS keys in namespace `aidlink`:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `gps_enable` | bool | `true` | Allow GPS to be selected at all. |
| `gps_pref` | int | `0` (feed) | Preferred source: 0 = feed, 1 = GPS. |

`gps_pref` defaults to **feed** so existing units behave exactly as they do today until the operator
deliberately switches.

## Error handling

- Sentences failing the NMEA checksum are dropped and counted into `csum_errors`, surfaced in
  `/status` as a wiring/EMI health gauge.
- UART errors and silence are indistinguishable by design: both age `last_rx_ms`, which stales the
  source and triggers fallback.
- `present` but `fix == GPS_FIX_NONE` is never chosen as live.
- Partial or split sentences are reassembled by a line accumulator; over-long lines are discarded.
- No GPS pins → subsystem inert, no task, no allocation.

## Testing

**Host** (`host_test/`, alongside the existing 12 suites):

- `test_nmea.c` — GGA/RMC/GSA/GSV parsing seeded with sentences **captured from the real module on
  2026-07-25**, covering the no-fix case (`$GNGGA,,,,,,0,00,99.99,,,,,,*56`), the 3D fix
  (`$GNGGA,012952.00,2217.59913,S,16626.35477,E,1,06,1.30,22.3,M,56.6,M,,*66`), multi-talker GSV,
  bad checksums, empty fields and truncated lines.
- `test_pos_choose.c` — the six-row arbitration truth table above.
- Quality-mapping table: `(fix, hdop, sats) → fv_gq_t` across every boundary.

**On-target:** both sources live; forced fallback each way; button gestures; all five colours;
and a no-GPS board (Board 2 or 4) confirming the portal is unchanged and no GPS task exists.

## Out of scope

- `UBX-CFG-VALSET` configuration (dynamic model, nav rate, constellation selection).
- Using PPS for time discipline — it is read and reported only.
- GNSS on the T3-S3. RX 16 / TX 12 would work there; PPS would not. Not wired today.
- Powering the module on battery: the header 5 V pin is VBUS, so GNSS dies on JST battery. A boost
  or a low-dropout module is required before this ships as a flight-critical source.
