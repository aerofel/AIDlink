# GNSS Hardware — u-blox MAX-M8Q Receiver

_Written 2026-07-26. Chipset figures are quoted from the u-blox **MAX-M8 series data
sheet, UBX-15031506 R06 (4-Dec-2023)**, not from memory. The carrier-board details are
read off the silkscreen of the physical unit — items that could not be confirmed
electrically are flagged in [Unverified](#unverified--verify-before-soldering)._

## At a glance

| | |
|---|---|
| Module | **u-blox MAX-M8Q-0-10**, ROM **SPG 3.01**, MAX form factor 9.7 × 10.1 × 2.5 mm |
| Carrier | Raspberry Pi HAT-format breakout: u.FL antenna, **CP2102** USB-UART + micro-USB, A/B/C routing jumpers |
| Band | **L1 only** — single frequency. No L2, no L5 |
| Constellations | GPS · GLONASS · BeiDou · Galileo · QZSS · SBAS, **3 concurrent** |
| Interfaces | UART + DDC (I²C, slave only). **No SPI, no USB** on the module |
| Protocols | NMEA 0183 v4.0 · UBX · RTCM 2.3 input |
| Supply | **2.7–3.6 V**, 23 mA tracking @ 3 V |
| Wired to | Board 3 (LilyGO T-Display-S3) — RX **16**, TX **12**, PPS **21**, 9600 baud |

---

## 1. The chipset — MAX-M8Q

### Constellations and signals

| Signal | Frequency |
|---|---|
| GPS L1C/A, SBAS L1C/A, QZSS L1C/A + L1-SAIF, Galileo E1B/C | 1575.42 MHz |
| GLONASS L1OF | ~1602 MHz |
| BeiDou B1I | 1561.098 MHz |

- 72-channel u-blox M8 engine.
- Concurrent reception of **up to 3 GNSS** — GPS + Galileo together with *either* BeiDou *or* GLONASS.
- **Power-on default: GPS + GLONASS + QZSS + SBAS.**
- ⚠️ **Galileo is disabled by default.** The data sheet is explicit: it must be enabled by sending `UBX-CFG-GNSS`. Free extra satellites on the same band — worth turning on.

### Performance (data sheet Table 1)

| Metric | GPS + GLONASS | GPS only | GLONASS | BeiDou |
|---|---|---|---|---|
| Horizontal accuracy (CEP) | **2.5 m** | 2.5 m | 4 m | 3 m |
| Max navigation update rate | **10 Hz** | 18 Hz | 18 Hz | 18 Hz |
| TTFF — cold | 26 s | 29 s | 30 s | 34 s |
| TTFF — hot | 1 s | 1 s | 1 s | 1 s |
| TTFF — aided | 2 s | 2 s | 2 s | 3 s |
| Sensitivity — tracking & nav | **−167 dBm** | −166 dBm | −166 dBm | −160 dBm |
| Sensitivity — reacquisition | −160 dBm | −160 dBm | −156 dBm | −157 dBm |
| Sensitivity — cold start | −148 dBm | −148 dBm | −145 dBm | −143 dBm |

- Velocity accuracy **0.05 m/s** · heading accuracy **0.3°**
- Receiver chain noise figure **3.5 dB**
- Operating temperature **−40 … +85 °C**

### Timepulse (PPS)

- One timepulse output, configurable **0.25 Hz … 10 MHz**, default 1 PPS, rising-edge synchronised, 100 ms pulse.
- Accuracy **30 ns RMS**, 60 ns @ 99% — the MAX-M8Q carries a **TCXO** (the cheaper M8C uses a plain crystal and is materially worse).

### Operational limits ✈️

| Limit | Value |
|---|---|
| Dynamics | **≤ 4 g** |
| Altitude | **50,000 m** |
| Velocity | **500 m/s** |

A jet at FL410 (≈12,500 m) and 300 m/s ground speed sits comfortably inside all three.
These are the u-blox limits; the COCOM export restriction is what they implement.

### Electrical

| | MAX-M8Q @ 3 V |
|---|---|
| VCC | **2.7 – 3.6 V** (the 1.65 V figure belongs to the M8C, not this part) |
| Icc acquisition | 26 mA |
| Icc tracking, continuous | 23 mA |
| Icc tracking, power-save | 6.2 mA |
| Icc peak | 67 mA |

### Interfaces and protocols

- **UART** and **DDC** (I²C-compatible, Fast Mode, **slave only**) — both carry all protocols.
- **No SPI and no USB** on the MAX-M8 package. The USB port on the carrier board belongs to the CP2102, not the GNSS.
- **NMEA 0183 v4.0** (v2.1 / 2.3 / 4.1 selectable), **UBX** binary, **RTCM 2.3** input (messages 1, 2, 3, 9) for DGPS.
- ⚠️ **RTCM corrections cannot be used together with SBAS.**

### Firmware storage — ROM, not flash

`MAX-M8Q-0-10` is a **ROM SPG 3.01** part. There is **no flash**:

- Firmware cannot be updated.
- Configuration persists only in **battery-backed RAM** via `V_BCKP`. With no backup cell, **every setting is lost at power-down**.
- AssistNow Autonomous works, but u-blox notes flash-based receivers perform better for it.

### Features present

Geofencing · odometer · message integrity protection (MD5 signing of UBX messages) ·
spoofing detection · AssistNow Online / Offline / Autonomous · OMA SUPL compliant.

### Features **not** present

❌ RTK ❌ raw carrier-phase output ❌ dead reckoning ❌ dual-band (L2/L5).
Those need M8P / M8T, or the F9 / M10 generations.

### Antenna

- Data sheet: *"MAX-M8Q provides best performance for passive and active antenna designs."*
- Active antenna minimum gain **15 dB** (to offset cable loss).
- Pin 13 is **`LNA_EN`** (antenna control) on the M8C/Q. Pin 15 `V_ANT` — the built-in
  antenna bias supply and supervisor — is **MAX-M8W only**. So the MAX-M8Q itself does
  **not** bias an active antenna; that has to come from the carrier board.
- **No antenna, no fix.** Connect the u.FL pigtail before concluding anything is broken.

---

## 2. The carrier board

Raspberry Pi HAT footprint (40-pin header, 4 mounting holes), but nothing on it is
electrically Pi-specific — the GNSS is plain 3.3 V TTL NMEA.

**On board:** MAX-M8Q · u.FL antenna connector · Silicon Labs **CP2102** USB-UART bridge ·
micro-USB (silkscreened `USB TO UART`) · a TSSOP-20 logic device · 4 status LEDs
(`PWR`, `RXD`, `TXD`, `PPS`) · the A/B/C jumper block.

### Breakout row (bottom edge, 11 pads)

Reading the silkscreen the right way up:

```
3V3 · VCC · 5V  ‖  5V · GND · RXD · TXD · INT · SDA · SCL · PPS
└── supply select ──┘   └────────── signal breakout ──────────┘
```

`INT` is the module's `EXTINT`; `SDA`/`SCL` are the DDC (I²C) port; `PPS` is the timepulse.
The left trio looks like a supply-select jumper — **feed only one of `3V3` / `5V`, never both.**

### A/B/C jumper block

Two jumpers, one per UART direction, on a 2-column header:

| Position | Routing |
|---|---|
| **A** | USB ~ GNSS — CP2102 talks to the receiver |
| **B** | Pi ~ GNSS — header pins 8/10 talk to the receiver |
| **C** | USB ~ Pi — bridge passes through, GNSS isolated |

**As received, both jumpers were on A.** For ESP32 use, **pull both jumpers off** so the
CP2102 cannot drive the module's RXD line against the ESP32's TX.

---

## 3. Wiring to the ESP32

Target is Board 3, LilyGO T-Display-S3 — the only fleet unit with GNSS pins assigned
(`firmware-idf/main/board.c`, `PROF_TDISPLAY_S3`).

| HAT pad | ESP32-S3 GPIO | `board_t` field | Note |
|---|---|---|---|
| `5V` | 5V / VBUS | — | feeds the carrier's regulator |
| `GND` | GND | — | common ground, mandatory |
| `TXD` | **16** | `gps_rx` | module NMEA out → ESP32 in |
| `RXD` | **12** | `gps_tx` | ESP32 out → module in (UBX config) |
| `PPS` | **21** | `gps_pps` | optional; **Board-3 only** |

GPIO 16 and 12 are the only pins free across the whole fleet — on the classic
ESP32-WROVER, 16/17 are the PSRAM lines, so opening a UART there blind is destructive.
On the T3-S3, GPIO 21 is QWIIC SCL *and* LoRa DIO3, which is why PPS is Board-3-only.

Baud is **9600 8N1** (`GPS_BAUD`, `firmware-idf/main/gps.c`), matching the u-blox default.

### Alternative: I²C instead of UART

The module speaks UBX/NMEA over DDC at address **`0x42`** on the `SDA`/`SCL` pads. Viable,
but needs a new transport in `gps.c` — the current driver is UART-only.

---

## 4. Consequences for the firmware

`firmware-idf/main/gps.c` is currently **receive-only** — it opens the UART and parses,
and never transmits. That means the module runs its power-on defaults: GPS+GLONASS+QZSS+SBAS,
1 Hz, **portable** dynamic model, 9600 baud NMEA. Three things follow:

1. **Set the dynamic platform model.** Default "portable" assumes low dynamics. For flight,
   send `UBX-CFG-NAV5` with `dynModel = 7` (airborne < 2 g), or 6 for < 1 g. Left on portable,
   the nav filter fights climbs, descents and turns.
   ⚠️ **`UBX-CFG-NAV5` is correct for *this* module and wrong for the other one** — see
   [Two different receivers](#5-two-different-receivers--do-not-share-config-code) below.
2. **Re-send configuration at every boot.** ROM part, no flash, and no backup cell visible on
   the carrier — nav rate, dynModel, constellation selection and baud rate do **not** survive a
   power cycle.
3. **9600 baud caps the update rate.** Default NMEA at 1 Hz fits comfortably; above ~2 Hz you
   must raise the baud rate or disable the chatty sentences (GSV, GSA, VTG).

### Regional note — South Pacific / Nouméa

SBAS has **no coverage** over the South Pacific (WAAS is North America, EGNOS Europe,
MSAS Japan, GAGAN India). Leaving SBAS enabled costs nothing but buys nothing here.
**Enabling Galileo is the real win** — more satellites, same L1 band, no extra hardware.

---

## 5. Two different receivers — do not share config code

The project now has **two GNSS modules with incompatible configuration protocols**. Whatever
drives them must branch on `UBX-MON-VER`, never assume.

| | **This HAT** | **Bench module (2026-07-25)** |
|---|---|---|
| Silkscreen | `MAX-M8Q-0-10` | `NEO-M8N-0-10` — **counterfeit** |
| `MON-VER` reports | ✅ **verified 2026-07-26** — `ROM CORE 3.01 (107888)`, `hwVersion=00080000`, `FWVER=SPG 3.01`, `PROTVER=18.00` → **genuine M8** | `hwVersion=000A0000`, `PROTVER=34.10`, `ROM SPG 5.10` → **M10-class silicon** |
| Dynamic model set via | **`UBX-CFG-NAV5`**, `dynModel` field | **`CFG-VALSET`** — `CFG-NAVSPG-DYNMODEL` (`0x20110021`), value 8 |
| Legacy `UBX-CFG-*` | supported | ⚠️ **does not exist on that part** |
| Firmware | mask ROM, no update | mask ROM, no update |

A real M8N would report `00080000` / PROTVER 18–20.x / `ROM CORE 3.01`. The bench part
reports none of those — hence the counterfeit call. See `LEARNING.md`, 2026-07-25.

**Also carried over from that bench session** (may or may not apply to this HAT, but check):

- The bench module's **active antenna has a 3.0 V floor** and its AMS1117 (1.1 V dropout) only
  produced ~2.2 V from the 3V3 rail. Signature: **AGC pinned at 11 %** in `UBX-MON-RF` — which
  is the *low*-AGC direction, i.e. too much input power, **not** the high AGC of a disconnected
  antenna. On 5 V, AGC recovered to 30–45 %.
- The header 5 V pin is **VBUS**, so a 5 V-fed GNSS **dies on JST battery**. Needs a boost, a
  low-dropout module, or a passive antenna before it can be a dependable position source.
- **`UBX-MON-RF` is the diagnostic that matters** — NMEA alone cannot distinguish interference
  from no signal.
- **Patch antennas are directional and the labelled face is the BASE.** Label-up cost 12–15 dB;
  label-down on a metal tray under open sky gave 6 sats, HDOP 1.30, 3D fix.

---

## 6. Bench check — 2026-07-26

Jumpers on **A**, micro-USB into the Mac. Enumerates as **Silicon Labs CP2102**
(`10C4:EA60`, serial `0001`) on **`/dev/cu.usbserial-0001`**, streaming NMEA at **9600 8N1**.

⚠️ Board 1 (classic ESP32) uses the same CP2102 and also claims `usbserial-0001` — the port
name alone does **not** identify which is plugged in. Read the stream to tell them apart.

Read-only UBX polls, nothing written:

```
MON-VER   SW  ROM CORE 3.01 (107888)      HW 00080000
          FWVER=SPG 3.01   PROTVER=18.00
          GPS;GLO;GAL;BDS;SBAS;IMES;QZSS
CFG-GNSS  trkChHw=32 trkChUse=32
          GPS ✅   SBAS ✅   QZSS ✅   GLONASS ✅
          Galileo ❌   BeiDou ❌   IMES ❌
CFG-NAV5  dynModel=0 (portable)   fixMode=3 (auto 2D/3D)
CFG-RATE  measRate=1000 ms → 1.00 Hz
MON-HW    agcCnt 5928/8191 (72%)   noisePerMS 99   jamInd 55/255
          antenna status OK, power ON
```

**Confirmed genuine.** `hwVersion=00080000` + `PROTVER=18.00` + `ROM CORE 3.01` is the real-M8
signature — so **legacy `UBX-CFG-*` works here**, demonstrated by CFG-NAV5 answering the poll.
Every data-sheet default is confirmed on the actual silicon.

☞ `trkChHw` reports **32 tracking channels**. The "72-channel" figure in u-blox marketing is the
acquisition/search engine, not concurrent tracking channels — don't expect 72 anywhere in `MON-*`.

**No signal at all.** NMEA showed `GGA` quality `0`, `GSA` mode `A,1`, and both `GPGSV`/`GLGSV`
at `1,1,00` — **zero satellites in view**, not merely no fix:

```
$GNGGA,,,,,,0,00,99.99,,,,,,*56
$GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99*2E
$GPGSV,1,1,00*79      $GLGSV,1,1,00*65
$GNRMC,,V,,,,,,,,,,N*4D
```

`jamInd` 55/255 and `noisePerMS` 99 rule out interference. Front end is clean and starved.

### Antenna moved outdoors — no change at all

Re-tested with the antenna outside, 60 s of NMEA plus a 12-sample AGC series:

```
t+10..60s   fix=0  sats=00  HDOP=99.99   GPGSV inView=00  GLGSV inView=00   RMC=V
AGC         5928 5928 5928 5928 5928 5928 5928 5928 5928 5928 5928 5928
            min=5928  max=5928  spread=0          ← frozen
noisePerMS  98 (varies)      jamInd 54–55 (varies)
```

**`agcCnt` is frozen at exactly 5928** — identical indoors and outdoors, zero spread over
minutes. `noisePerMS` and `jamInd` *do* dither, so `MON-HW` is genuinely refreshing; the gain
loop simply never responds. A live AGC always moves by tens of counts. **Nothing is coupling
into `RF_IN`** — the RF path is open. The receiver is healthy; signal never arrives.

### ⚠️ `aStatus=OK` does NOT mean an antenna is connected

`UBX-CFG-ANT` reports `flags=0x001b`:

| Bit | Meaning | State |
|---|---|---|
| `svcs` | antenna supply voltage control | ✅ on |
| `scd` | **short** detection | ✅ on |
| `ocd` | **open** detection | ❌ **OFF** |
| `pdwnOnSCD` | power down on short | ✅ on |
| `recovery` | auto recovery | ✅ on |

Open-circuit detection is **disabled**, so the receiver *cannot* detect a missing or
disconnected antenna. `aStatus=OK` means only "no short" — it is **not** evidence that an
antenna is present or that bias reaches it. Do not use it as an antenna-present check.

### Antenna swap — AGC is bit-identical across two different antennas

| Antenna | `agcCnt` | `noisePerMS` | sats in view | `NAV-SAT` |
|---|---|---|---|---|
| Known-good active patch (the one that gave 6 sats / HDOP 1.30 on 2026-07-25) | **5928** | 98–99 | 00 | `numSvs=0` |
| Small pigtail antenna | **5928** | 92–93 | 00 | `numSvs=1`, `GPS-10 cno=0 el=-91 qual=1` (blind search, zero energy) |

Swapping the antenna changes the load and noise power at `RF_IN`, so an adaptive AGC **must**
move. It does not shift by a single count. **The antenna is ruled out as the variable.**

Two hypotheses remain:

- **(A) Both antennas are active and the carrier supplies no bias.** MAX-M8Q has no `V_ANT`.
  An unpowered active antenna presents the same dead load whatever its type — which explains
  identical AGC exactly, and explains `aStatus=OK` (an unpowered LNA is DC-blocked, no short
  to detect). **Board fine; needs a passive antenna or a bias-T.**
- **(B) The on-board RF path is broken** between the u.FL and `RF_IN` — cracked u.FL joint,
  damaged SAW or matching network. **Board faulty.**

**Discriminator — DC volts on the u.FL centre ↔ shield, antenna unplugged:**
`~3–5 V` → bias exists, so suspect **(B)**, inspect the u.FL joints under magnification.
`0 V` → **(A)** confirmed; the receiver will never drive an active antenna as wired.

---

## 7. Wiring confirmed on Board 2 — 2026-07-26

HAT wired to the **ESP32-S3 devkit (Board 2, MAC `e8:3d:c1:f7:a2:58`)**, jumpers on **B**,
`PROF_S3_DEVKIT` given `.gps_rx=16 .gps_tx=12 .gps_pps=21`, flashed over the CH343 port.

```
I (1111) board: e8:3d:c1:f7:a2:58 -> esp32s3-devkit (display=none led=ws2812)
I (2381) gps:   listening on UART1 rx=16 tx=12 pps=21 @ 9600 baud

/status → "gps": { "present": true, "enabled": true, "fix": 1,
                   "sats_used": 0, "sats_view": 0, "hdop": 99.99,
                   "pps_us": 0, "csum_err": 0 }
```

✅ **`present=true` with `csum_err=0` validates the whole serial path at once** — jumper
position B, TX/RX orientation, and 9600 baud. `present` only goes true on a
checksum-valid sentence, so a swapped pair or a wrong jumper would have left it false.

`sats_view=0` / `hdop=99.99` match what was measured directly over the CP2102 — the RF
fault is unchanged and is now the **only** thing between this and a position.
`pps_us=0` is expected, not a fault: `CFG-TP5` emits no pulse until GNSS lock, so GPIO21
cannot be validated until the antenna works.

☞ **Flashing Board 2:** native-USB `/dfu` does **not** work — the ROM never enumerates a
CDC port. Use the second USB port (**WCH CH343**, `1A86:55D3`). esptool auto-reset works,
no BOOT+RST, and the stub is fine there. See `LEARNING.md` 2026-07-26.

---

## Unverified — verify before soldering

Read off a photograph, not measured. Buzz these out with a multimeter first:

- [ ] Are the breakout `RXD`/`TXD` pads on the **module** side or the **Pi** side of the A/B/C
      jumper block? (Continuity from `TXD` to the module's TXD pin vs. to header pin 10.)
- [ ] Which of `3V3` / `5V` is the **supply input** vs. a regulator output?
- [ ] Exact identity of the TSSOP-20 device — level shifter, bus switch, or the A/B/C matrix.
- [x] ~~Confirm the shipped firmware with `UBX-MON-VER`~~ — ✅ 2026-07-26, genuine M8, SPG 3.01,
      PROTVER 18.00. `UBX-CFG-NAV5` is the right API for this part.
- [ ] Does the carrier actually bias an active antenna, and at what voltage? `CFG-ANT` shows
      `svcs` enabled, so supply control exists — but **`aStatus=OK` proves nothing** because
      `ocd` (open detect) is off. Measure the rail against the antenna's 3.0 V floor.
- [ ] **RF path is open — unresolved.** AGC frozen at 5928 indoors *and* outdoors means no
      signal reaches `RF_IN`. Decisive next test: **unplug the antenna and re-read `agcCnt`.**
      Still 5928 → the antenna was never electrically in the path (u.FL not seated, or a dead
      pigtail). Value jumps → the antenna is connected and the fault is the antenna or its bias.
      Also confirm the attached antenna is **L1 / 1575 MHz**, not a 868/915 MHz LoRa whip from
      the T3-S3 kit — that swap produces exactly this signature.

## Sources

- [MAX-M8 series data sheet — UBX-15031506 R06](https://content.u-blox.com/sites/default/files/documents/MAX-M8-FW3_DataSheet_UBX-15031506.pdf)
- [u-blox MAX-M8 series product page](https://www.u-blox.com/en/product/max-m8-series)
- Related: [`ESP32-BOARDS.md`](ESP32-BOARDS.md) · [`firmware-idf/main/gps.c`](firmware-idf/main/gps.c) · [`firmware-idf/main/board.c`](firmware-idf/main/board.c)
