# ESP32 Boards — Detected Specs & USB-Networking Feasibility

_Generated 2026-07-05 from live `esptool` + macOS USB enumeration; Board 3 added
2026-07-06, Board 4 added 2026-07-23. All silicon values read live from the chips,
not assumed._

## At a glance

| | **Board 1 — Classic ESP32** | **Board 2 — ESP32-S3 devkit** | **Board 3 — LilyGO T-Display-S3** | **Board 4 — LilyGO T3-S3 (LoRa)** |
|---|---|---|---|---|
| Silicon (detected) | ESP32-D0WD-V3, rev **v3.1** | ESP32-S3 (QFN56), rev **v0.2** | ESP32-S3 (QFN56), rev **v0.2** | ESP32-S3 (QFN56), rev **v0.2** — **`S3FH4R2`** (flash + PSRAM in package) |
| CPU | Dual-core Xtensa **LX6**, up to 240 MHz | Dual-core Xtensa **LX7**, up to 240 MHz | Dual-core Xtensa **LX7**, up to 240 MHz | Dual-core Xtensa **LX7**, up to 240 MHz |
| SRAM | 520 KB | 512 KB | 512 KB | 512 KB |
| **PSRAM** | none (this module) | **8 MB embedded** (octal, "R8") | **8 MB embedded** (octal, "R8") | **2 MB embedded, QUAD** (AP_3v3, "R2") ⚠️ not octal |
| **Flash** | **4 MB** | **16 MB** ("N16") | **16 MB** (quad, Winbond `ef/4018`) | **4 MB embedded** (quad, XMC `46/4016`) |
| Wi-Fi | 802.11 b/g/n, 2.4 GHz | 802.11 b/g/n, 2.4 GHz | 802.11 b/g/n, 2.4 GHz | 802.11 b/g/n, 2.4 GHz |
| Bluetooth | **BT Classic (BR/EDR) + BLE 4.2** | **BLE 5.0 only** (no Classic) | **BLE 5.0 only** (no Classic) | **BLE 5.0 only** (no Classic) |
| **Display** | — | — | **1.9″ ST7789 170×320 IPS**, i80 8-bit bus | 0.96″ **SSD1306** 128×64 OLED, I²C **`0x3C`** on SDA 18 / SCL 17 (probed) |
| **Sub-GHz radio** | — | — | — | **LoRa module on SPI2** (CS 7 / RST 8 / BUSY 34) |
| **Native USB** | **No** — needs external UART bridge | **Yes** — USB-OTG + USB-Serial-JTAG | **Yes** — the **only** port (no UART bridge) | **Yes** — the **only** port (no UART bridge) |
| Crystal | 40 MHz | 40 MHz | 40 MHz | 40 MHz |
| MAC (base) | `44:1d:64:f5:9f:88` | `e8:3d:c1:f7:a2:58` | `d0:cf:13:32:2f:48` | `1c:db:d4:bd:1f:1c` |
| Attached as | `/dev/cu.usbserial-0001` | `/dev/cu.usbmodem5AE60430151` | `/dev/cu.usbmodem1101` (stock) → **USB-NCM** once AIDlink runs | `/dev/cu.usbmodem1CDBD4BD1F1C1` (factory FW) → `/dev/cu.usbmodem101` in ROM download mode |
| USB bridge in path | **Silicon Labs CP2102** (VID `0x10C4`) | **WCH CH343** (VID `0x1A86`, PID `0x55D3`) | **none** — native USB-Serial-JTAG (VID `0x303A`, PID `0x1001`) | **none** — native USB (VID `0x303A`, PID `0x1001`, product string `LilyGo TLora_T3S3_V1`) |
| Current firmware | **AIDlink v9** (huge_app, 3 MB app) | AIDlink ESP-IDF | **AIDlink ESP-IDF + flight display** (flashed 2026-07-06) | **LilyGO factory demo** (Arduino, IDF v4.4.7, built 2024-03-05) — AIDlink not flashed |

---

## Board 1 — Classic ESP32 (the AIDlink unit)

**Detected by esptool 5.3.0:**
```
Chip type:   ESP32-D0WD-V3 (revision v3.1)
Features:    Wi-Fi, BT, Dual Core, 240MHz, Vref calibration in eFuse, Coding Scheme None
Crystal:     40MHz     MAC: 44:1d:64:f5:9f:88     Flash: 4MB
```

- **Core:** 2× Xtensa LX6 @ 240 MHz, 520 KB SRAM, 448 KB ROM.
- **Radio:** Wi-Fi 802.11 b/g/n (2.4 GHz) + **Bluetooth Classic (BR/EDR) and BLE 4.2**.
- **USB:** **None on-chip.** The micro-USB port is wired to a **Silicon Labs CP2102 USB-to-UART bridge**, so the host only ever sees a **serial port**. No USB device-class capability of any kind.
- **Flash/PSRAM:** 4 MB flash (mfr `0x68`), no PSRAM.
- **Rev v3.1** is the mature ECO-fixed silicon (the "good" classic ESP32).
- **Role here:** runs the AIDlink Wi-Fi bridge (AP + STA + NAPT + ADBP), firmware **v9** on the `huge_app` partition scheme (≈38 % of a 3 MB app slot).

**USB-as-network verdict:** ❌ impossible. No native USB; the cable is a UART. Only IP option is
PPP-over-serial (dial-up speed, cannibalizes the console) — see the earlier analysis.

---

## Board 2 — ESP32-S3 (the new unit, N16R8)

**Detected by esptool 5.3.0:**
```
Chip type:   ESP32-S3 (QFN56) (revision v0.2)
Features:    Wi-Fi, BT 5 (LE), Dual Core, 240MHz, Embedded PSRAM 8MB (AP_3v3)
Crystal:     40MHz     MAC: e8:3d:c1:f7:a2:58     Flash: 16MB
```

- **Core:** 2× Xtensa LX7 @ 240 MHz, 512 KB SRAM, plus 128-bit vector instructions (accelerates ML/DSP).
- **Memory:** **16 MB flash + 8 MB embedded PSRAM** — a very high-end "N16R8" module. Huge headroom vs. Board 1.
- **Radio:** Wi-Fi 802.11 b/g/n (2.4 GHz) + **BLE 5.0** — note **no Bluetooth Classic** (S3 dropped it).
- **USB — the important part:** the S3 has a **native USB-OTG controller (USB 1.1 Full-Speed, 12 Mbps)** *and* a built-in USB-Serial-JTAG. This chip **can** enumerate as a USB device class — including a **network adapter (CDC-NCM / CDC-ECM / RNDIS)**.

### ⚠️ But you're plugged into the wrong port right now

The attached port enumerated as **WCH CH343 (VID `0x1A86`)** — that's a **USB-to-UART bridge**, i.e. the board's **"UART"/"COM"** connector. It is *not* the S3's native USB. If you were on the native port, macOS would show an **Espressif device (VID `0x303A`)**, typically `303A:1001` (USB-Serial-JTAG) or a custom TinyUSB PID.

➡️ **To use USB networking you must plug the USB-C into the S3 board's other port — the one labeled "USB"** (wired to GPIO19/20 = D-/D+). Most dual-USB-C S3 devkits have both a "UART" (CH343) and a "USB" (native) connector.

---

## Can the S3's USB-C replace Wi-Fi? (NAT the cable to the uplink)

**Hardware: YES.** The S3's native USB-OTG can be a **USB-NCM** network gadget (best class for macOS,
which supports NCM & ECM natively). Full-Speed USB gives ~**6–9 Mbps usable**, which **meets or beats
the ESP32's real Wi-Fi-through-NAT throughput** (~2–6 Mbps). So functionally it can truly replace the
Wi-Fi hop: `Mac ⇄ USB-NCM ⇄ [S3] ⇄ NAPT ⇄ upstream Wi-Fi ⇄ Internet`, exactly like the AP clients today.

**Two real caveats:**

1. **Port:** must use the native-USB connector (VID `0x303A`), not the CH343 UART port you're on now.
2. **Software stack:** the Arduino ESP32 core used by this project exposes **no USB-network class**
   (its USB library offers only HID, MIDI, MSC, CDC-serial, Vendor, Audio — verified). USB networking
   requires **ESP-IDF**, where the ready-made building block is the official
   **`examples/peripherals/usb/device/tusb_ncm`** (USB-NCM device) combined with `esp_netif` NAT to
   the Wi-Fi STA. Practically this means porting AIDlink from the `.ino`/arduino-cli build to an
   ESP-IDF project (the Wi-Fi/NAT/ADBP/web logic carries over; the build system and USB bring-up are new).

### Feasibility summary

| Approach | This S3 board? | Speed | Effort |
|---|---|---|---|
| **USB-NCM gadget + NAT** (native USB port) | ✅ yes | ~6–9 Mbps (≈ Wi-Fi) | ESP-IDF port required |
| USB-ECM / RNDIS variants | ✅ yes (NCM preferred on macOS) | same | same |
| PPP-over-serial (CH343 UART port) | ✅ works but pointless here | ~0.09 Mbps | medium |
| Stock Arduino USB-network object | ❌ not supported | — | — |

**Bottom line:** the S3 is the right chip for "USB-C instead of Wi-Fi," at genuinely useful speed — but
it needs (a) the **native USB port**, and (b) an **ESP-IDF firmware** (Arduino can't do USB networking).

---

## Board 3 — LilyGO T-Display-S3 (the display unit)

**Detected by esptool 4.11 (2026-07-06):**
```
Chip type:   ESP32-S3 (QFN56) (revision v0.2)
Features:    WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)
Crystal:     40MHz     MAC: d0:cf:13:32:2f:48     Flash: 16MB (quad, ef/4018)
USB mode:    USB-Serial/JTAG
```

- Same S3 N16R8-class silicon as Board 2, in LilyGO's T-Display-S3 form factor.
- **Display:** 1.9″ ST7789 IPS, 170×320 (used landscape 320×170), on the S3's
  LCD peripheral over an **Intel-8080 8-bit parallel bus**. Factory pin map:
  D0..D7 = GPIO 39/40/41/42/45/46/47/48, WR 8, RD 9, DC 7, CS 6, RST 5,
  backlight 38, LCD power enable 15. Panel RAM offset: y-gap 35, colors inverted.
  ⚠️ **GPIO48 here is LCD data D7** — on Board 2 it's the WS2812 status LED, which
  is why the firmware gates the LED and the display per board (`board.c`).
- **Single USB-C, no UART bridge:** the stock board enumerates as the S3's
  USB-Serial-JTAG (`303A:1001`, iSerial = MAC). Unlike Board 2, **flashing via the
  native USB works fine** on this unit (bootloader + app verified 2026-07-06).
  Once AIDlink runs, TinyUSB turns that port into **USB-NCM** — the serial port
  disappears (that's normal). To reflash: hit the auth-gated **`/dfu`** endpoint
  (reboots into the ROM downloader, no buttons), or hold **BOT** + tap RST.
  ⚠️ After flashing over native USB the chip **re-latches into download mode**
  on soft resets (`boot:0x23` forced download) — finish with **one RST tap**.
  A USB replug is not a power cycle when a battery is on the JST connector.
- **Console:** none in practice (UART0 is only on the header pins 43/44).
  Verify health over the cable instead: NCM lease, `ping 172.20.1.1`, `/status`,
  and the display bring-up trace in `/log`.
- **Buttons:** RST, BOT (GPIO0) and user key (GPIO14; unused by firmware).
- **RAM note:** internal SRAM is the scarce resource on this build — LVGL pool
  capped at 16 KB and a single 20-line draw buffer (see LEARNING.md 2026-07-06).
- **Runs:** AIDlink ESP-IDF with the onboard **flight display** (tail, flight
  number, DEP→ARR, NM to arrival, UTC offset + local time at position).

---

## Board 4 — LilyGO T3-S3 (the LoRa unit)

**Detected by esptool 4.11 (2026-07-23):**
```
Chip type:   ESP32-S3 (QFN56) (revision v0.2)
Features:    WiFi, BLE, Embedded Flash 4MB (XMC), Embedded PSRAM 2MB (AP_3v3)
Crystal:     40MHz     MAC: 1c:db:d4:bd:1f:1c
Flash:       4MB quad (mfr 46 / dev 4016, XMC), 3.3 V, quad set in eFuse
USB mode:    USB-Serial/JTAG
eFuse:       PKG_VERSION 0, WAFER v0.2, BLK_VERSION_MINOR 4, FLASH_TYPE quad, PSRAM_VENDOR AP_3v3
```

- **Module:** **ESP32-S3FH4R2** — flash *and* PSRAM are **inside the package**, not
  separate chips. That is the whole story of this board: **4 MB flash, 2 MB PSRAM,
  both quad-line**. It is the *smallest*-memory S3 of the three S3 boards here, not
  another N16R8.
- **Radio:** Wi-Fi 802.11 b/g/n (2.4 GHz) + **BLE 5.0** (no Bluetooth Classic).
- **Single USB-C, no UART bridge** — same topology as Board 3. Stock, the factory
  firmware presents a **TinyUSB CDC** port named `LilyGo TLora_T3S3_V1`
  (`303A:1001`, iSerial = MAC) at `/dev/cu.usbmodem1CDBD4BD1F1C1`.
  ⚠️ **esptool cannot reset that port into download mode** — the running app owns
  USB, so `--before default_reset` / `usb_reset` both fail with *"No serial data
  received"*. What worked: a **1200-baud touch** (open the CDC port at 1200 with DTR
  low, then close) — the app reboots into the ROM downloader and re-enumerates as
  **`/dev/cu.usbmodem101`**, where esptool connects immediately. Manual fallback is
  hold **BOOT** + tap **RST**.
- **Sub-GHz LoRa module** on its own SPI bus (**SPI2**), separate from the SD-card SPI:
  SCK 5, MISO 3, MOSI 6, **CS 7, RST 8, BUSY 34**, IRQ 33/36 depending on the radio.
  ⚠️ **The radio variant is not electrically detectable from the host** — LilyGO ships
  T3-S3 with SX1276 (868/915), SX1262, SX1280 (2.4 GHz) or LR1121. Read the silkscreen
  on the shield, or probe the SPI ID register from firmware, before writing any driver.
- **OLED — confirmed live 2026-07-23** by a bit-banged I²C scan flashed to this
  unit: **SSD1306 acks at `0x3C` on SDA 18 / SCL 17**. Both lines idle high; the
  swapped orientation (SDA 17 / SCL 18) acks nothing, and the QWIIC bus
  (SDA 10 / SCL 21) is empty. A dark panel out of the box is *normal* — an
  SSD1306 powers up with its driver off (`0xAE`), and LilyGO's factory demo
  never initialises it.
- **Quad PSRAM — confirmed live 2026-07-23**: built with `CONFIG_SPIRAM_MODE_QUAD`,
  the chip reports `INIT OK, 2048 KB`, leaving **2045 KB on the SPI heap and
  377 KB internal**. The octal build would have silently yielded zero.
- **Other on-board peripherals** (LilyGO T3-S3 pin map, from the Arduino core variant
  `lilygo_t3_s3_lr1121`; only the I²C pins above are probed):
  QWIIC I²C1 **SDA 10, SCL 21**; microSD on SPI3
  (SCK 14, MISO 2, MOSI 11, CS 13); LED **GPIO37**; battery-sense ADC **GPIO1**;
  BOOT button **GPIO0**; UART0 on header pins **TX 43 / RX 44**.
- **Factory firmware still on it** (read live from the app descriptor at `0x10020`):
  `arduino-lib-builder`, ESP-IDF **v4.4.7**, built **2024-03-05 12:12:53**.
  Its partition table is `nvs 20K / otadata 8K / ota_0 2368K / ota_1 640K / spiffs 1M`.

### ⚠️ AIDlink does not build for this board as-is

Two hard blockers in `firmware-idf/sdkconfig`, both a direct consequence of the
S3FH4R2 package:

| Setting | Current | This board needs |
|---|---|---|
| `CONFIG_SPIRAM_MODE_OCT` | **octal** (set for Boards 2/3) | **`CONFIG_SPIRAM_MODE_QUAD`** — octal init fails here |
| PSRAM size assumed | 8 MB | **2 MB** |

`CONFIG_SPIRAM_IGNORE_NOTFOUND=y` means it would still *boot* — silently with **no
PSRAM at all**, on a board whose internal SRAM budget is already the scarce resource
(see Board 3's LVGL notes). Flash is fine: the project targets 4 MB
(`CONFIG_ESPTOOLPY_FLASHSIZE_4MB`) with a single 3 MB `factory` app, which fits.
`board.c` also keys features off the base MAC, so `1c:db:d4:bd:1f:1c` needs an entry
there (no WS2812, no ST7789 display) before the display/LED code does the right thing.

---

_All silicon / flash / PSRAM / MAC values above are read live from the chips, not assumed._
