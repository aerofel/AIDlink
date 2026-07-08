<!-- SPDX-License-Identifier: Apache-2.0 -->
# AIDlink — ESP-IDF firmware (dual-target, USB-cable networking)

A native **ESP-IDF** rewrite of the AIDlink firmware. One codebase, two build
targets:

| Target | Board | Adds |
|---|---|---|
| `esp32` | classic ESP32 (4 MB) | Wi-Fi bridge (AP + STA + NAT + ADBP + web + mDNS) |
| `esp32s3` | ESP32-S3 (16 MB) | **all of the above + USB-NCM cable networking + RGB status LED** |

On the S3, a host plugged into the **native USB** port gets a DHCP address and
internet through the S3's NAT to the upstream Wi-Fi — i.e. the USB-C cable works
like joining the Wi-Fi AP, but wired. It **coexists** with the Wi-Fi AP.

Feature parity with the original Arduino sketch (`../firmware/aidlink/`): the web
config portal is reproduced pixel-for-pixel, and the ARINC-834 ADBP feed, the
position poller (Viasat/Panasonic/custom + emulator), auth, and mDNS all match.

> **EXPERIMENTAL — not certified, not for operational use.** The position output
> has no integrity guarantee; never use it for navigation.

## Network model

```
 upstream Wi-Fi ──STA──┐
                        │ NAPT
   SoftAP 172.20.1.1/26 ┤◄── Wi-Fi clients (both targets)
   USB-NCM 172.20.2.1/29┘◄── USB cable host (S3 only)
   DNS forwarder :53 relays AP + USB client queries to the live uplink resolver
```

## Build & flash

Prerequisite: **ESP-IDF v5.3.x** installed (e.g. `~/esp/esp-idf`), plus its
bundled `cmake`/`ninja` (`python3 $IDF_PATH/tools/idf_tools.py install cmake ninja`).

```bash
. ~/esp/esp-idf/export.sh
cd firmware-idf
./flash-idf.sh esp32            # classic board via CP2102
./flash-idf.sh esp32s3          # S3 via its CH343 UART port
./flash-idf.sh esp32s3 <port> monitor
```

**⚠️ ESP32-S3 flashing:** flash + monitor over the board's **CH343 UART** port,
**not** the native-USB port. On this hardware the native-USB path boot-loops the
2nd-stage bootloader (stock `hello_world` fails identically). At runtime the
native USB is the USB-NCM network device, and the console is on UART.

`flash-idf.sh` force-cleans the build dir when switching target (a stale `build/`
otherwise points esptool at the wrong `--chip`).

## Configuration

Browse to `http://172.20.1.1/` (Wi-Fi AP) or `http://172.20.2.1/` (USB cable).
Default login **admin / password** — change it on the Security card. Set the
uplink SSID/password on the Uplink Wi-Fi card; the device reboots to apply (the
"Saved ✓" page waits for it to come back, then returns you to the config).

Defaults: uplink DHCP on, poll interval 1 s, aircraft `F-XXXX` / `A320`,
AP `AIDlink` `172.20.1.1/26`, ADBP `24000`, EFB DataStreamPort `51000`.

## Position feed (ADBP, ARINC 834)

TCP `:24000` command channel — `getAvionicParameters`, `subscribeAvionicParameters`
(pushes to the client's `<publishport>`), `unSubscribe`. Sources: Viasat /
Panasonic / custom URL, or the built-in emulator (Emulator card).

The **AID Web API** on `:80` (`getAPIVersion`, `getWiFiAPStatus`, `getAoIPStatus`,
`getAcarsStatus`, `cmdReboot`) answers both **GET and POST** — EFBs such as
Jeppesen FliteDeck probe with POST, so GET-only would fail AID detection.

## Onboard RGB status LED (ESP32-S3)

The S3's WS2812 (GPIO48) shows status on one pixel:

| LED | Meaning |
|---|---|
| flashing orange | scanning for Wi-Fi |
| solid green | uplink connected |
| flashing red | searching / not connected |
| brief blue flash (~250 ms) | a position frame was sent to an EFB |

The board's other small LEDs (power, UART/USB activity) are hardwired and not
software-controllable. If your board's RGB LED is on a different pin, change
`LED_GPIO` in `statusled.c`.

The LED is gated per-board in `board.c` (eFuse MAC → features): on the LilyGO
T-Display-S3 GPIO48 is LCD data line D7, so the WS2812 driver must not run there.

## Onboard flight display (LilyGO T-Display-S3)

Boards flagged `has_display` in `board.c` drive the 1.9″ ST7789 (320×170
landscape, Intel-8080 8-bit bus) via `esp_lcd` + LVGL 9 (`esp_lvgl_port`,
S3-only managed components). `display.c` renders, at 1 Hz from the shared
ownship state:

- **tail** (cyan) and **flight number** (top row),
- **DEP → ARR** route, large, greyed while the fix is invalid,
- **distance to arrival** in NM (great-circle to the destination from the
  embedded gazetteer `airports.c` — Aircalin network + Pacific/Asia/AU/NZ,
  IATA and ICAO codes),
- **zone + UTC (`12:30:12z`) + local time at position** (bottom row). The
  offset comes from an embedded IANA-derived timezone grid (`tzdb.c`, 1°
  resolution, DST-aware, :30/:45 offsets included). UTC needs no internet:
  the clock is disciplined by the position feed's own HTTP `Date` header
  (poller.c), with the fix timestamp as fallback and SNTP as an opportunistic
  bonus when the uplink really reaches the internet.

The same binary runs on display-less S3 boards (`display_start()` is a no-op
there); the classic ESP32 build contains zero LVGL/esp_lcd code.

**Memory budget (hard-won):** the display shares internal SRAM with
WiFi + TinyUSB + the L2 bridge. LVGL's pool is capped at 16 KB
(`CONFIG_LV_MEM_SIZE_KILOBYTES` — the 64 KB default is a *static* BSS array
that starved the DMA draw buffer and made `lvgl_port_add_disp` return NULL =
black screen), and rendering uses a single 320×20 partial buffer. Display
bring-up mirrors every step into the web `/log` ring — the only "console"
this board has.

**T-Display-S3 reflashing:** the single USB-C is USB-NCM while AIDlink runs.
Two routes into the ROM downloader:
1. **Remote (preferred):** `curl -b <cookies> http://172.20.1.1/dfu` — the
   auth-gated `/dfu` endpoint forces download-boot and restarts. Then
   `idf.py -p /dev/cu.usbmodem* flash`.
2. **Physical:** hold **BOT** (GPIO0), tap RST, release.

Either way, after flashing over the native USB the S3 usually **re-latches
into download mode** on soft reset (`boot:0x23` = forced download) — finish
with **one physical RST tap**. A USB replug is *not* a power cycle if a
battery is connected. No UART bridge: logs only on the UART0 header pins
(43/44), or via `/log` over the network.

## Tests

Pure logic is host-unit-tested with plain `clang` (no on-target harness needed):

```bash
cd firmware-idf
CJ=~/esp/esp-idf/components/json/cJSON
clang -Imain -o /tmp/t host_test/test_config_subnet.c   main/config_util.c   && /tmp/t
clang -Imain -o /tmp/t host_test/test_dnsfwd_remap.c    main/dnsfwd_util.c   && /tmp/t
clang -Imain -o /tmp/t host_test/test_web_util.c        main/web_util.c      && /tmp/t
clang -Imain -o /tmp/t host_test/test_adbp_frame.c      main/adbp_frame.c -lm && /tmp/t
clang -Imain -o /tmp/t host_test/test_geo.c             main/geo.c        -lm && /tmp/t
clang -Imain -o /tmp/t host_test/test_airports.c        main/airports.c main/geo.c -lm && /tmp/t
clang -Imain -o /tmp/t host_test/test_tzdb.c            main/tzdb.c main/tzdb_data.c -lm && /tmp/t
clang -Imain -o /tmp/t host_test/test_derive.c          main/derive.c main/geo.c -lm && /tmp/t
clang -Imain -I$CJ -o /tmp/t host_test/test_poller_sources.c main/poller_sources.c $CJ/cJSON.c -lm && /tmp/t
```

## Source layout (`main/`)

| File | Responsibility |
|---|---|
| `aidlink_main.c` | boot: NVS, config, bring up net/usb/web/adbp/poller/mdns/led |
| `config.*` / `config_util.c` | NVS config (+ host-tested subnet math) |
| `netcore.*` | Wi-Fi STA + SoftAP + NAPT + DHCP/DNS offer; scan coordination; client/AP status |
| `dnsfwd.*` / `dnsfwd_util.c` | UDP :53 DNS forwarder (+ host-tested id remap) |
| `usb_ncm.*` | S3 USB-NCM netif + DHCP + NAPT + host tracking (guarded by USB-OTG) |
| `web.*` / `web_util.c` | esp_http_server config portal (v9-exact) + AID Web API (+ host-tested parsing) |
| `auth.*` | salted SHA-256 login + persistent session cookie |
| `pos.*` | mutex-guarded shared ownship state |
| `adbp.*` / `adbp_frame.c` | ARINC-834 ADBP server (+ host-tested wire format) |
| `poller.*` / `poller_sources.c` / `geo.*` | position poller, JSON parsers, geo (all host-tested) |
| `services.*` | mDNS advertisement |
| `log.*` | device traffic-log ring buffer (web `/log`) |
| `statusled.*` | onboard RGB status LED (boards that have one) |
| `board.*` | board identity by eFuse MAC → per-board hardware (LED, display) |
| `display.*` | T-Display-S3 flight display (esp_lcd i80 ST7789 + LVGL) |
| `airports.*` | embedded IATA/ICAO → lat/lon gazetteer (host-tested) |
| `tzdb.*` / `tzdb_data.c` | 1° world timezone grid + offset transitions, generated from IANA data (host-tested; regenerate ~2028) |
| `derive.*` | GS/track from successive fixes: movement-gated, filtered, wrap-safe (host-tested) |
| `font_arrow.c` | generated single-glyph ➤ font for the display route line (`tools/gen_arrow_font.py`) |

## Relationship to the Arduino sketch

The original Arduino sketch under `../firmware/aidlink/` remains the proven,
shipping build. This ESP-IDF project is the go-forward firmware (it adds USB-cable
networking and the status LED, only possible on the S3 via ESP-IDF) and reaches
feature parity with the sketch. The one item still to confirm on real hardware is
the live upstream Wi-Fi → NAT → internet path (bench testing so far has had no
live uplink present).

See `../LEARNING.md` for the design decisions, hardware gotchas (S3 native-USB
boot loop, NAPT Kconfig rename, HTTP header limit, iOS session cookies, …), and
the milestone history.
