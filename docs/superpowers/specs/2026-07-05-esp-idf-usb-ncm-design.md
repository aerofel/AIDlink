# AIDlink on ESP-IDF — USB-cable networking (USB-NCM) + dual-target

**Status:** approved design (2026-07-05)
**Supersedes build:** current Arduino/`arduino-cli` firmware (`firmware/aidlink/aidlink.ino`, v9)

## Goal

Rewrite the AIDlink firmware natively in **ESP-IDF**, as **one codebase with two build
targets**:

- **`esp32`** (classic ESP32-D0WD-V3, 4 MB flash) — Wi-Fi bridge, **feature-parity with v9**.
- **`esp32s3`** (ESP32-S3, 16 MB flash / 8 MB PSRAM) — same feature set **plus USB-NCM cable
  networking**: a host (Mac) plugged into the S3's native USB gets a DHCP address and internet
  through the S3's NAT to the upstream Wi-Fi, exactly like a Wi-Fi-AP client.

All USB code is compile-guarded by `CONFIG_SOC_USB_OTG_SUPPORTED`; the classic target omits it and
behaves identically to today.

## Hardware context (live-detected)

| | Board 1 (classic) | Board 2 (S3) |
|---|---|---|
| Chip | ESP32-D0WD-V3 rev v3.1 | ESP32-S3 rev v0.2 |
| Flash / PSRAM | 4 MB / none | 16 MB / 8 MB |
| Native USB | **no** (CP2102 UART bridge) | **yes** — USB-OTG FS + USB-Serial-JTAG |
| Console/flash port | CP2102 `/dev/cu.usbserial-0001` | CH343 UART `/dev/cu.usbmodemXXXX` |
| Native USB port | — | Espressif `0x303A` `/dev/cu.usbmodem101` |

The S3's native USB currently enumerates as USB-Serial-JTAG; the firmware will reconfigure it as a
TinyUSB **CDC-NCM** network device. Console + flashing stay on the **CH343 UART port**, so we don't
lose logs/flashing when native USB becomes NCM.

## Non-goals (YAGNI)

- No RNDIS/CDC-ECM (CDC-NCM only; native on macOS and modern Linux/Win11).
- No USB networking on the classic ESP32 (physically impossible).
- No new product features — exact v9 behavior parity, nothing added, nothing dropped.
- No OTA (matches current huge_app / no-OTA choice).

## Network architecture

```
                         ┌─────────────────────────── ESP32 / ESP32-S3 ──────────────────────────┐
 upstream Wi-Fi  ◄──STA──┤  esp_netif(STA)  ──┐                                                    │
 (Viasat/Aircalin)       │                    │  NAPT (lwIP IP_NAPT) on egress                     │
                         │  esp_netif(AP)  ◄──┤  172.20.1.1/26  DHCP pool  ── Wi-Fi clients (both) │
                         │  esp_netif(USB) ◄──┘  172.20.2.1/29  DHCP       ── USB host (S3 only)    │
                         │  DNS forwarder :53 (relays to live STA resolver) for AP + USB clients   │
                         └────────────────────────────────────────────────────────────────────────┘
```

- **Uplink:** Wi-Fi STA to upstream, DHCP or static (unchanged from v9).
- **Downstream A — Wi-Fi SoftAP (both targets):** `172.20.1.1/26`, DHCP pool, NAPT → STA. As v9.
- **Downstream B — USB-NCM (S3 only):** dedicated netif `172.20.2.1/29`; host leases `172.20.2.2`.
  NAPT → STA. **Coexists** with the AP; both are downstream clients of the same uplink.
- **NAT:** lwIP `IP_NAPT` enabled for both downstream netifs; translation on the STA egress.
- **DNS:** port the v9 on-device forwarder. Each downstream client is handed **its own gateway IP**
  as DNS (`172.20.1.1` for AP, `172.20.2.1` for USB); the forwarder relays to the live STA resolver,
  so DNS follows uplink reconnects and never goes stale in a lease.

## USB-NCM subsystem (S3 only)

- **Class:** TinyUSB device, **CDC-NCM**. Base pattern: ESP-IDF `examples/peripherals/usb/device/
  tusb_ncm` (`tinyusb_net_init()` + a dedicated `esp_netif`).
- **Autodetect:** TinyUSB mount + NCM link-up callbacks drive it. On host-attach / link-up → start the
  DHCP server on the USB netif and enable NAPT. On detach → tear down. With no cable, the S3 is a pure
  Wi-Fi bridge (no behavioral difference from the classic target).
- **Console/flashing:** keep on UART (CH343). Native USB is dedicated to NCM. `sdkconfig` sets the
  console to UART, not USB-Serial-JTAG, on the S3 target.
- **Addressing:** the USB netif is a distinct subnet (`172.20.2.0/29`) so routing/NAPT for the two
  downstream interfaces is unambiguous.

## Feature parity — reimplemented natively (both targets)

| v9 (Arduino) | ESP-IDF replacement |
|---|---|
| `WebServer` config portal (:80) | `esp_http_server` — same pages, fields, save/reboot flow |
| ADBP ARINC-834 server (TCP 24000 + push subs) | raw TCP via `esp_netif`/lwIP sockets — same wire protocol |
| Position poller (Viasat/Panasonic/custom) | `esp_http_client` + `esp-tls` (HTTPS) |
| `Preferences` config | NVS (`nvs_flash`) — **same keys + cfgVer migration** |
| Auth (salted SHA-256) | `mbedtls` SHA-256 — same salt/hash scheme |
| mDNS (`ESPmDNS`) | `mdns` component |
| Serial logging / log dump | UART console + in-RAM ring buffer (as v9) |
| Position simulator | ported as-is |

## Build system, partitions, prerequisites

- **Prerequisite:** install **ESP-IDF v5.3.x** (`release/v5.3`) with `esp32,esp32s3` toolchains
  (not currently installed; running now).
- **Project:** CMake ESP-IDF app. `idf.py set-target esp32` / `idf.py set-target esp32s3`.
- **sdkconfig:** `sdkconfig.defaults` (common) + `sdkconfig.defaults.esp32s3` (TinyUSB + NCM enabled,
  console→UART, PSRAM enabled). Classic target has no USB keys.
- **Partitions:** per-target CSV. Classic = single ~3 MB app on 4 MB (huge_app equivalent, no OTA/FS).
  S3 = large app on 16 MB, PSRAM enabled.
- **Tooling:** rework `tools/flash.sh` around `idf.py -B build.<target> build flash monitor`,
  auto-selecting target and keeping the CP2102/CH343 UART ports for console/flash.

## Repository layout (proposed)

```
firmware-idf/                 # new ESP-IDF project (Arduino sketch kept until parity is proven)
  CMakeLists.txt
  sdkconfig.defaults
  sdkconfig.defaults.esp32s3
  partitions.esp32.csv
  partitions.esp32s3.csv
  main/
    CMakeLists.txt
    aidlink_main.c            # app entry, wifi/ap/nat/dns bring-up
    net_nat_dns.c/.h          # NAPT + DNS forwarder (shared)
    usb_ncm.c/.h              # S3-only, guarded by SOC_USB_OTG_SUPPORTED
    web.c/.h                  # esp_http_server config portal
    adbp.c/.h                 # ARINC-834 ADBP server + push
    poller.c/.h               # position source client
    config.c/.h               # NVS config + migration
    ...
```

## Verification

- **Cable (S3):** Mac plugs native USB → new CDC-NCM interface → leases `172.20.2.2` →
  `nslookup`, `curl`, `iperf3` succeed through the S3 NAT to the uplink.
- **Coexistence:** a Wi-Fi-AP client and the USB host both reach the internet simultaneously.
- **Autodetect:** unplug/replug the cable — NCM netif + DHCP + NAPT come up/tear down cleanly;
  Wi-Fi bridge unaffected.
- **Classic regression:** `esp32` target builds and runs with v9-identical behavior (web UI, ADBP,
  poller, DNS forwarder, mDNS); no USB code compiled in.
- **ADBP:** position feed reaches a test/EFB client over both Wi-Fi and cable paths.

## Risks & mitigations

- **Big-bang rewrite regression risk** (accepted by user): mitigate by keeping the Arduino sketch in
  the tree until the ESP-IDF build reaches verified parity, and by a per-subsystem verification list.
- **NAPT with two downstream netifs:** confirm lwIP `IP_NAPT` handles AP + USB simultaneously; the
  `tusb_ncm` example proves USB→STA, AP→STA is proven by v9 — validate the combination early.
- **Console loss on native USB:** avoided by keeping console on UART; documented in flashing tooling.

## Open items to confirm with user before implementation-plan

None outstanding — the five design assumptions (CDC-NCM only, 172.20.2.1/29, console-on-UART,
ESP-IDF v5.3.x, exact v9 parity) were accepted.
