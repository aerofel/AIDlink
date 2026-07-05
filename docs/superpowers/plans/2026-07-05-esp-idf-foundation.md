# AIDlink ESP-IDF Foundation (Milestone 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A bootable, dual-target ESP-IDF firmware that is a working Wi-Fi→NAT bridge (STA uplink + SoftAP + NAPT + on-device DNS forwarder) and, on the ESP32-S3, delivers internet to a USB-cable host via a USB-NCM network device with attach/detach autodetect.

**Architecture:** One ESP-IDF app, two build targets (`esp32`, `esp32s3`). A shared network core brings up STA + SoftAP + NAPT + a UDP DNS forwarder. An S3-only module (compile-guarded by `CONFIG_SOC_USB_OTG_SUPPORTED`) adds a TinyUSB CDC-NCM device on a second downstream `esp_netif`, with DHCP + NAPT, activated by TinyUSB mount/link callbacks. Config for this milestone comes from NVS with seeded build-time defaults (the web UI arrives in Milestone 2).

**Tech Stack:** ESP-IDF v5.3.x, esp_wifi, esp_netif, lwIP (IP_NAPT), esp_tinyusb / tinyusb_net (CDC-NCM), nvs_flash, FreeRTOS, Unity (host + on-target tests).

## Global Constraints

- ESP-IDF **v5.3.x** (`release/v5.3`), installed at `~/esp/esp-idf`; source `. ~/esp/esp-idf/export.sh` before any `idf.py`.
- **Two targets only:** `esp32` (classic, 4 MB, no USB) and `esp32s3` (16 MB / 8 MB PSRAM, native USB).
- **All USB code compile-guarded by `CONFIG_SOC_USB_OTG_SUPPORTED`** — the `esp32` build must contain zero USB references.
- **Console + flashing stay on UART** (CP2102 on classic; CH343 UART port on S3). Native USB is NCM-only; do **not** route the console to USB-Serial-JTAG on the S3.
- **Subnets:** SoftAP `172.20.1.1/26` (DHCP pool from `172.20.1.2`); USB-NCM `172.20.2.1/29` (host leases `172.20.2.2`). AP DNS offer = `172.20.1.1`; USB DNS offer = `172.20.2.1`.
- **NAPT:** `CONFIG_LWIP_IP_NAPT=y`; enable on each downstream netif (AP always; USB when attached).
- New project lives in **`firmware-idf/`**; the existing Arduino sketch (`firmware/aidlink/`) stays untouched until parity is reached in later milestones.
- Reference (authoritative) for USB-NCM glue: `~/esp/esp-idf/examples/peripherals/usb/device/tusb_ncm` — cross-check names/APIs against this exact example after install.
- Commit after every task. Do not push (owner pushes explicitly).

---

## File Structure

```
firmware-idf/
  CMakeLists.txt                     # project() + target guard
  sdkconfig.defaults                 # common: NAPT, console→UART, custom partitions
  sdkconfig.defaults.esp32s3         # PSRAM, TinyUSB NCM, USB console off
  partitions.csv                     # nvs + phy_init + 3MB factory app (fits 4MB & 16MB)
  main/
    CMakeLists.txt                   # srcs + idf_component.yml deps
    idf_component.yml                # espressif/esp_tinyusb (S3), managed deps
    aidlink_main.c                   # app_main: config load, net bring-up, usb bring-up
    config.h / config.c              # Config struct + NVS load/save/seed (subset for M1)
    netcore.h / netcore.c            # STA + SoftAP + NAPT bring-up
    dnsfwd.h / dnsfwd.c              # UDP :53 forwarder task (shared by AP + USB)
    usb_ncm.h / usb_ncm.c            # S3-only CDC-NCM netif + DHCP + NAPT + autodetect
  host_test/                         # host (linux) Unity tests for pure logic
    CMakeLists.txt
    test_dnsfwd_remap.c
    test_config_subnet.c
```

---

### Task 1: ESP-IDF project scaffold + dual-target build

**Files:**
- Create: `firmware-idf/CMakeLists.txt`, `firmware-idf/main/CMakeLists.txt`, `firmware-idf/main/aidlink_main.c`, `firmware-idf/sdkconfig.defaults`, `firmware-idf/sdkconfig.defaults.esp32s3`, `firmware-idf/partitions.csv`

**Interfaces:**
- Produces: a buildable ESP-IDF app whose `app_main()` prints a boot banner `"[aidlink-idf] boot <target>"`.

- [ ] **Step 1: Create the minimal project files**

`firmware-idf/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aidlink)
```

`firmware-idf/main/CMakeLists.txt`:
```cmake
idf_component_register(SRCS "aidlink_main.c"
                       INCLUDE_DIRS ".")
```

`firmware-idf/main/aidlink_main.c`:
```c
#include <stdio.h>
#include "esp_system.h"
#include "esp_log.h"
#include "sdkconfig.h"
static const char *TAG = "aidlink";
void app_main(void) {
    ESP_LOGI(TAG, "[aidlink-idf] boot %s", CONFIG_IDF_TARGET);
}
```

`firmware-idf/partitions.csv`:
```csv
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x6000
phy_init, data, phy,     0xf000,   0x1000
factory,  app,  factory, 0x10000,  0x300000
```

`firmware-idf/sdkconfig.defaults`:
```
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_LWIP_IP_NAPT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
```

`firmware-idf/sdkconfig.defaults.esp32s3`:
```
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
```

- [ ] **Step 2: Build for the classic ESP32**

Run:
```bash
. ~/esp/esp-idf/export.sh
cd firmware-idf && idf.py set-target esp32 && idf.py build
```
Expected: build succeeds; ends with `Project build complete.`

- [ ] **Step 3: Build for the S3**

Run: `idf.py set-target esp32s3 && idf.py build`
Expected: build succeeds. (Leave target on `esp32s3` for later USB tasks; you can switch back anytime.)

- [ ] **Step 4: Commit**
```bash
git add firmware-idf/CMakeLists.txt firmware-idf/main firmware-idf/sdkconfig.defaults* firmware-idf/partitions.csv
git commit -m "idf: scaffold dual-target ESP-IDF project"
```

---

### Task 2: Config layer (NVS + seeded defaults) with host-testable subnet math

**Files:**
- Create: `firmware-idf/main/config.h`, `firmware-idf/main/config.c`, `firmware-idf/host_test/CMakeLists.txt`, `firmware-idf/host_test/test_config_subnet.c`
- Modify: `firmware-idf/main/CMakeLists.txt` (add `config.c`)

**Interfaces:**
- Produces:
  - `typedef struct { char sta_ssid[33]; char sta_pass[65]; char ap_ssid[33]; char ap_pass[65]; uint8_t ap_ip[4]; uint8_t ap_prefix; uint16_t ap_lease_min; uint8_t ap_dhcp_count; char ap_client_dns[16]; uint8_t usb_ip[4]; uint8_t usb_prefix; bool napt_enable; } aidlink_cfg_t;`
  - `void cfg_load(aidlink_cfg_t *c);` — reads NVS namespace `"aidlink"`, applies seeded defaults for any missing key.
  - `esp_err_t cfg_save(const aidlink_cfg_t *c);`
  - `uint32_t cfg_netmask_from_prefix(uint8_t prefix);` — pure; returns host-order mask (e.g. prefix 26 → 0xFFFFFFC0).

- [ ] **Step 1: Write the failing host test**

`firmware-idf/host_test/test_config_subnet.c`:
```c
#include "unity.h"
#include "config.h"
TEST_CASE("prefix 26 -> mask 0xFFFFFFC0", "[config]") {
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFC0, cfg_netmask_from_prefix(26));
}
TEST_CASE("prefix 29 -> mask 0xFFFFFFF8", "[config]") {
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFF8, cfg_netmask_from_prefix(29));
}
TEST_CASE("prefix 0 -> mask 0", "[config]") {
    TEST_ASSERT_EQUAL_HEX32(0x00000000, cfg_netmask_from_prefix(0));
}
```

`firmware-idf/host_test/CMakeLists.txt`:
```cmake
idf_component_register(SRCS "test_config_subnet.c" "test_dnsfwd_remap.c"
                       INCLUDE_DIRS "../main"
                       REQUIRES unity)
```

- [ ] **Step 2: Create `config.h` with the struct + `cfg_netmask_from_prefix` declaration** (full struct from Interfaces above; declare the three functions).

- [ ] **Step 3: Run the host test — verify it fails to link**

Run:
```bash
cd firmware-idf && idf.py --preview set-target linux
idf.py -C host_test build 2>&1 | tail -5
```
Expected: FAIL — undefined reference to `cfg_netmask_from_prefix`.

- [ ] **Step 4: Implement `cfg_netmask_from_prefix` in `config.c`**
```c
#include "config.h"
uint32_t cfg_netmask_from_prefix(uint8_t prefix) {
    if (prefix == 0) return 0;
    if (prefix >= 32) return 0xFFFFFFFFu;
    return 0xFFFFFFFFu << (32 - prefix);
}
```

- [ ] **Step 5: Run host test — verify pass**

Run: `idf.py -C host_test build && ./host_test/build/*.elf` (or `idf.py -C host_test build && idf.py -C host_test monitor` per IDF linux target). Expected: 3 tests PASS.

- [ ] **Step 6: Implement `cfg_load`/`cfg_save` (NVS)** in `config.c`, using `nvs_open("aidlink", ...)`, `nvs_get_str/blob`, seeding these defaults when a key is absent: `sta_ssid=""`, `ap_ssid="AIDlink"`, `ap_pass="88888888"`, `ap_ip={172,20,1,1}`, `ap_prefix=26`, `ap_lease_min=120`, `ap_dhcp_count=60`, `ap_client_dns=""`, `usb_ip={172,20,2,1}`, `usb_prefix=29`, `napt_enable=true`. Add `config.c` to `main/CMakeLists.txt`.

- [ ] **Step 7: Commit**
```bash
git add firmware-idf/main/config.* firmware-idf/main/CMakeLists.txt firmware-idf/host_test
git commit -m "idf: config layer (NVS + seeded defaults) with subnet-math unit tests"
```

---

### Task 3: DNS-forwarder txn-id remap logic (host-tested) + task

**Files:**
- Create: `firmware-idf/main/dnsfwd.h`, `firmware-idf/main/dnsfwd.c`, `firmware-idf/host_test/test_dnsfwd_remap.c`
- Modify: `firmware-idf/main/CMakeLists.txt` (add `dnsfwd.c`)

**Interfaces:**
- Produces:
  - `#define DNSFWD_SLOTS 16`
  - `typedef struct { uint8_t cip[4]; uint16_t cport; uint16_t oid; uint16_t sid; uint32_t t0; bool used; } dnsfwd_pend_t;`
  - `uint16_t dnsfwd_make_sid(int slot, uint16_t *seq);` — pure; encodes `slot` in top nibble, `(*seq)++ & 0x0FFF` in low bits.
  - `int dnsfwd_slot_of(uint16_t sid);` — pure; returns `(sid>>12)&0x0F`.
  - `void dnsfwd_start(esp_netif_t *sta_netif, const char *client_dns_override);` — starts the UDP :53 relay task.

- [ ] **Step 1: Write the failing host test**

`firmware-idf/host_test/test_dnsfwd_remap.c`:
```c
#include "unity.h"
#include "dnsfwd.h"
TEST_CASE("sid encodes slot in top nibble", "[dnsfwd]") {
    uint16_t seq = 0;
    uint16_t sid = dnsfwd_make_sid(5, &seq);
    TEST_ASSERT_EQUAL_INT(5, dnsfwd_slot_of(sid));
}
TEST_CASE("seq varies low bits, slot stable", "[dnsfwd]") {
    uint16_t seq = 0x0FFE;
    uint16_t a = dnsfwd_make_sid(3, &seq);
    uint16_t b = dnsfwd_make_sid(3, &seq);
    TEST_ASSERT_EQUAL_INT(3, dnsfwd_slot_of(a));
    TEST_ASSERT_EQUAL_INT(3, dnsfwd_slot_of(b));
    TEST_ASSERT_NOT_EQUAL(a, b);
}
```

- [ ] **Step 2: Create `dnsfwd.h`** with the macros, struct, and the three function declarations from Interfaces (guard the `esp_netif_t` include with a forward `typedef struct esp_netif_obj esp_netif_t;` so the host test compiles without full IDF).

- [ ] **Step 3: Run host test — verify it fails** (`idf.py -C host_test build`; undefined reference to `dnsfwd_make_sid`).

- [ ] **Step 4: Implement the pure helpers in `dnsfwd.c`**
```c
uint16_t dnsfwd_make_sid(int slot, uint16_t *seq) {
    return (uint16_t)(((slot & 0x0F) << 12) | ((*seq)++ & 0x0FFF));
}
int dnsfwd_slot_of(uint16_t sid) { return (sid >> 12) & 0x0F; }
```

- [ ] **Step 5: Run host test — verify pass** (2 tests PASS).

- [ ] **Step 6: Implement `dnsfwd_start`** — a FreeRTOS task that opens a UDP socket bound to `0.0.0.0:53`, and one upstream socket; on each client query, allocate a slot, remap the DNS transaction id via `dnsfwd_make_sid`, forward to the STA resolver (`esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, ...)`, or `client_dns_override` when non-empty), and relay replies back restoring `oid`. Port the v9 forwarder semantics (3 s expiry, oldest-slot eviction). Guard-compile safely (this function body is only built in the IDF app, not the host test — wrap IDF-only code in `#ifndef HOST_TEST` or keep it in a separate TU; simplest: put pure helpers first and the task after an `#include "lwip/sockets.h"` that host_test never compiles because it only lists the helpers TU — ensure `test_dnsfwd_remap.c` links only the helpers by keeping helpers in `dnsfwd_util.c`). Add sources to `main/CMakeLists.txt`.

- [ ] **Step 7: Commit**
```bash
git add firmware-idf/main/dnsfwd* firmware-idf/main/CMakeLists.txt firmware-idf/host_test/test_dnsfwd_remap.c
git commit -m "idf: DNS forwarder (txn-id remap unit-tested, relay task)"
```

---

### Task 4: Network core — STA + SoftAP + NAPT + DNS offer

**Files:**
- Create: `firmware-idf/main/netcore.h`, `firmware-idf/main/netcore.c`
- Modify: `firmware-idf/main/aidlink_main.c`, `firmware-idf/main/CMakeLists.txt`

**Interfaces:**
- Consumes: `aidlink_cfg_t` (Task 2), `dnsfwd_start` (Task 3).
- Produces:
  - `esp_netif_t *netcore_start(const aidlink_cfg_t *c);` — brings up Wi-Fi `APSTA`, STA connect, SoftAP with DHCP pool + DNS offer (`ap_ip`), enables NAPT on the AP netif, and starts the DNS forwarder. Returns the STA netif.
  - `esp_netif_t *netcore_sta_netif(void);`, `esp_netif_t *netcore_ap_netif(void);`

- [ ] **Step 1: Implement `netcore_start`** — `esp_netif_init`, event loop, `esp_wifi_init`, `WIFI_MODE_APSTA`; STA config from `c->sta_ssid/pass`; SoftAP config from `c->ap_ssid/pass`; set AP IP info from `c->ap_ip/ap_prefix`; configure DHCP server lease (`esp_netif_dhcps_option` REQUESTED_IP_ADDRESS from `ap_ip`+2, count `ap_dhcp_count`, lease `ap_lease_min`); set DNS MAIN = `ap_ip` and enable `OFFER_DNS`; `esp_netif_napt_enable(ap_netif)`; call `dnsfwd_start(sta_netif, c->ap_client_dns)`. Log a `[AP]`/`[STA]` summary line like v9.

- [ ] **Step 2: Wire into `app_main`**
```c
aidlink_cfg_t cfg; cfg_load(&cfg);
ESP_ERROR_CHECK(nvs_flash_init());
netcore_start(&cfg);
```

- [ ] **Step 3: Build + flash the classic ESP32, verify on hardware**

Run: `idf.py set-target esp32 build && idf.py -p /dev/cu.usbserial-0001 flash monitor`
Expected serial: `[aidlink-idf] boot esp32`, STA connect log, `[AP] ... NAPT ON`, `[DNS] forwarder up`. (Seed `sta_ssid`/`sta_pass` via a temporary default or `idf.py` build-time define for this test, e.g. Aircalin_Wifi.)

- [ ] **Step 4: Verify a Wi-Fi client gets internet through NAT** — join the `AIDlink` AP from a phone/laptop, confirm DNS + a web fetch work (uplink must be present). Note result in the commit body.

- [ ] **Step 5: Commit**
```bash
git add firmware-idf/main/netcore.* firmware-idf/main/aidlink_main.c firmware-idf/main/CMakeLists.txt
git commit -m "idf: network core (STA+SoftAP+NAPT+DNS offer) — Wi-Fi NAT bridge working"
```

---

### Task 5: USB-NCM cable networking (S3 only) with autodetect

**Files:**
- Create: `firmware-idf/main/usb_ncm.h`, `firmware-idf/main/usb_ncm.c`, `firmware-idf/main/idf_component.yml`
- Modify: `firmware-idf/main/aidlink_main.c`, `firmware-idf/main/CMakeLists.txt`, `firmware-idf/sdkconfig.defaults.esp32s3`

**Interfaces:**
- Consumes: `aidlink_cfg_t` (Task 2), `netcore_sta_netif()` (Task 4), `dnsfwd_start` (Task 3, reused for the USB subnet's DNS offer).
- Produces:
  - `void usb_ncm_start(const aidlink_cfg_t *c);` — no-op unless `CONFIG_SOC_USB_OTG_SUPPORTED`. Initializes TinyUSB in NCM mode, creates the USB `esp_netif` (`usb_ip/usb_prefix`), wires `tinyusb_net` rx/tx, and installs mount/link callbacks.
  - Autodetect: on NCM link-up → start DHCP server on the USB netif (offer DNS = `usb_ip`) + `esp_netif_napt_enable(usb_netif)`; on link-down → stop DHCP + disable NAPT.

- [ ] **Step 1: Add the managed dependency** — `firmware-idf/main/idf_component.yml`:
```yaml
dependencies:
  espressif/esp_tinyusb: "~1.4"
```

- [ ] **Step 2: Enable TinyUSB NCM in S3 sdkconfig** — append to `sdkconfig.defaults.esp32s3`:
```
CONFIG_TINYUSB_NET_MODE_NCM=y
```
(Confirm the exact key against `~/esp/esp-idf/examples/peripherals/usb/device/tusb_ncm/sdkconfig.defaults` after install; use whatever that example sets.)

- [ ] **Step 3: Implement `usb_ncm.c`** closely following `~/esp/esp-idf/examples/peripherals/usb/device/tusb_ncm/main/tusb_ncm_main.c`: create `esp_netif` with a custom inherent config for the USB interface, set static IP `usb_ip/usb_prefix`, `tinyusb_net_init(TINYUSB_USBDEV_0, ...)` with rx/free/init callbacks that bridge to `esp_netif_receive`, and a `tud_network_...`/link callback that triggers the DHCP+NAPT bring-up. Wrap the **entire** file body in `#if CONFIG_SOC_USB_OTG_SUPPORTED` / `#else` (empty `usb_ncm_start`) `#endif`. Add `usb_ncm.c` to `main/CMakeLists.txt`.

- [ ] **Step 4: Call it from `app_main`** (after `netcore_start`): `usb_ncm_start(&cfg);`

- [ ] **Step 5: Build both targets — verify the classic build has no USB**

Run:
```bash
idf.py set-target esp32 build
idf.py set-target esp32s3 build
```
Expected: both succeed. Confirm the esp32 `.map` has no `tinyusb` symbols: `grep -c tinyusb build/aidlink.map` → `0` after an `esp32` build.

- [ ] **Step 6: Flash the S3 (via CH343 UART port) and verify the cable**

Run: `idf.py set-target esp32s3 -p <CH343-UART-port> flash monitor`
Then plug the Mac into the S3 **native USB** port and check the host:
```bash
# new NCM interface appears; it should get 172.20.2.2
ifconfig | grep -B3 172.20.2
nslookup example.com 172.20.2.1
curl -m 10 -I http://example.com     # succeeds via S3 NAT (uplink present)
```
Expected: interface up, DNS resolves via the forwarder, HTTP reachable. Serial shows a `[USB] NCM link up` + DHCP + NAPT log.

- [ ] **Step 7: Verify autodetect + coexistence** — unplug the cable (serial: `[USB] link down`, teardown), replug (comes back). With the cable up, a Wi-Fi-AP client still reaches the internet simultaneously.

- [ ] **Step 8: Commit**
```bash
git add firmware-idf/main/usb_ncm.* firmware-idf/main/idf_component.yml firmware-idf/main/aidlink_main.c firmware-idf/main/CMakeLists.txt firmware-idf/sdkconfig.defaults.esp32s3
git commit -m "idf: USB-NCM cable networking on S3 (autodetect, coexists with AP)"
```

---

### Task 6: Flashing tooling for the dual-target IDF build

**Files:**
- Create: `firmware-idf/flash-idf.sh`

**Interfaces:**
- Produces: `./firmware-idf/flash-idf.sh <esp32|esp32s3> [port]` — sources `export.sh`, `idf.py set-target`, `build flash`, keeping the correct UART port default per target.

- [ ] **Step 1: Write `flash-idf.sh`**
```bash
#!/bin/bash
set -e
TGT="${1:?usage: flash-idf.sh <esp32|esp32s3> [port]}"
DEF_PORT_esp32=/dev/cu.usbserial-0001
DEF_PORT_esp32s3=/dev/cu.usbmodem101     # NOTE: use the CH343 UART port, not native USB
PORT="${2:-$(eval echo \$DEF_PORT_$TGT)}"
. ~/esp/esp-idf/export.sh
cd "$(dirname "$0")"
idf.py set-target "$TGT"
idf.py -p "$PORT" build flash
echo ">>> flashed $TGT on $PORT"
```

- [ ] **Step 2: Make executable + smoke test**

Run: `chmod +x firmware-idf/flash-idf.sh && ./firmware-idf/flash-idf.sh esp32`
Expected: builds and flashes the classic board.

- [ ] **Step 3: Commit**
```bash
git add firmware-idf/flash-idf.sh
git commit -m "idf: dual-target flashing helper"
```

---

### Task 7: Milestone integration verification + notes

**Files:**
- Modify: `LEARNING.md` (append M1 outcome)

- [ ] **Step 1: Run the full acceptance checklist and record actual results**
  - `esp32` target: boots, STA connects, AP client gets internet, DNS resolves. ✅/❌
  - `esp32s3` target: same as above, PLUS cable host gets `172.20.2.2` + internet; autodetect on unplug/replug; AP + cable simultaneous. ✅/❌
  - `esp32` build contains zero `tinyusb` symbols. ✅/❌
  - Host unit tests (config subnet, dnsfwd remap) pass. ✅/❌

- [ ] **Step 2: Append a dated `LEARNING.md` entry** summarizing what worked, any ESP-IDF API deviations from this plan (esp. the exact `tinyusb`/NCM sdkconfig keys and `tinyusb_net` callback signatures), and throughput measured over the cable (`iperf3`).

- [ ] **Step 3: Commit**
```bash
git add LEARNING.md
git commit -m "idf: milestone-1 acceptance verified (Wi-Fi bridge + USB-NCM cable)"
```

---

## Follow-on milestones (separate plans, later)

- **M2 — Web config portal** (`esp_http_server`): port the v9 settings pages/fields, save→reboot, auth (salted SHA-256), scan, status/JSON APIs. Adds runtime config for everything seeded in Task 2.
- **M3 — ADBP ARINC-834 server**: TCP 24000 + push subscriptions, framing options, EFB DataStreamPort.
- **M4 — Position poller + sources**: Viasat/Panasonic/custom via `esp_http_client`+esp-tls; simulator; stale→NCD.
- **M5 — Polish/parity**: mDNS, logging ring buffer + serial dump, config migration parity, partition tuning for S3 (use the 16 MB), retire the Arduino sketch once parity is signed off.

## Self-Review

- **Spec coverage (M1 scope):** STA uplink ✅(T4) · SoftAP+DHCP+NAPT ✅(T4) · DNS forwarder ✅(T3,T4) · USB-NCM + autodetect + coexistence ✅(T5) · dual-target one-codebase ✅(T1,T5) · console-on-UART ✅(constraints,T5,T6) · subnets 172.20.1/26 & 172.20.2/29 ✅(constraints,T2,T4,T5) · partitions ✅(T1). Web/ADBP/poller/auth/mDNS/log = deferred to M2–M5 (documented).
- **Placeholder scan:** no TBD/TODO; each code step shows code; Task 3 Step 6 and Task 5 Step 3 point at the exact IDF example file to translate rather than guessing unverifiable callback bodies — acceptable because the reference is a concrete artifact present after install.
- **Type consistency:** `aidlink_cfg_t` fields used consistently (T2 defines, T4/T5 consume); `dnsfwd_make_sid`/`dnsfwd_slot_of` names stable across T3 test + impl + T5 reuse; `netcore_sta_netif()` name matches T4 def and T5 use.
