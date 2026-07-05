<!-- SPDX-License-Identifier: Apache-2.0 -->
# AIDlink — ESP-IDF firmware (dual-target, USB-cable networking)

A native **ESP-IDF** rewrite of the AIDlink firmware. One codebase, two build
targets:

| Target | Board | Adds |
|---|---|---|
| `esp32` | classic ESP32 (4 MB) | Wi-Fi bridge (AP + STA + NAT + ADBP + web) |
| `esp32s3` | ESP32-S3 (16 MB) | **all of the above + USB-NCM cable networking** |

On the S3, a host plugged into the **native USB** port gets a DHCP address and
internet through the S3's NAT to the upstream Wi-Fi — i.e. the USB-C cable works
like joining the Wi-Fi AP, but wired. It **coexists** with the Wi-Fi AP.

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

## Configuration

Browse to `http://172.20.1.1/` (Wi-Fi AP) or `http://172.20.2.1/` (USB cable).
Default login **admin / password** — change it on the Security card.
Set the uplink SSID/password on the Uplink Wi-Fi card; the device reboots to apply.

## Position feed (ADBP, ARINC 834)

TCP `:24000` command channel — `getAvionicParameters`, `subscribeAvionicParameters`
(pushes to the client's `<publishport>`), `unSubscribe`. Sources: Viasat /
Panasonic / custom URL, or the built-in emulator (Emulator card).

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
clang -Imain -I$CJ -o /tmp/t host_test/test_poller_sources.c main/poller_sources.c $CJ/cJSON.c -lm && /tmp/t
```

## Source layout (`main/`)

| File | Responsibility |
|---|---|
| `aidlink_main.c` | boot: NVS, config, bring up net/usb/web/adbp/poller/mdns |
| `config.*` / `config_util.c` | NVS config (+ host-tested subnet math) |
| `netcore.*` | Wi-Fi STA + SoftAP + NAPT + DHCP/DNS offer |
| `dnsfwd.*` / `dnsfwd_util.c` | UDP :53 DNS forwarder (+ host-tested id remap) |
| `usb_ncm.*` | S3 USB-NCM netif + DHCP + NAPT (guarded by USB-OTG) |
| `web.*` / `web_util.c` | esp_http_server config portal (+ host-tested parsing) |
| `auth.*` | salted SHA-256 login + session cookie |
| `pos.*` | mutex-guarded shared ownship state |
| `adbp.*` / `adbp_frame.c` | ARINC-834 ADBP server (+ host-tested wire format) |
| `poller.*` / `poller_sources.c` / `geo.*` | position poller, JSON parsers, geo (all host-tested) |
| `services.*` | mDNS advertisement |

## Relationship to the Arduino sketch

The original Arduino sketch under `../firmware/aidlink/` remains the proven,
shipping build. This ESP-IDF project is the go-forward firmware (it adds USB-cable
networking, only possible on the S3 via ESP-IDF). It reaches feature parity with
the sketch; the one item still to confirm on real hardware is the live upstream
Wi-Fi → NAT → internet path (bench testing so far has had no live uplink).
