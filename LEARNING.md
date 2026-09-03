# AIDlink — Learning Journal

## 2026-09-04 — Poller lockups are the UPLINK, not the code; Viasat endpoint map

Same leg (VTBS→NWWW, F-ONET, ACI501). The feed died twice with grayed dep→arr
and no ETA. **I got the first diagnosis wrong — recording it so nobody re-runs it.**

- **WRONG: "the keep-alive handle wedges".** `poller.c` really does create `s_http`
  once (`if (s_http) return true;`) and only ever `close()` it, never
  `cleanup()`, so a wedged transport would be permanent. Plausible, and the flog
  showed lockups in **19 of 25 boots** (once 2 h 40 m straight). I shipped a
  rebuild-after-5-fails fix. **It fired 28 times and changed nothing.** Keep the
  fix — it closes a genuine latent hole — but it was not the cause.
- **RIGHT: the uplink Wi-Fi degrades and the poller is just its heaviest user.**
  The evidence that settles it:
  - `rssi=-83..-88`, and one window logged **`rssi=0`** — that is
    `esp_wifi_sta_get_ap_info()` failing, i.e. the STA had dropped entirely.
  - `maxms=7547 / 8022 / 10407` — these are **timeouts** against the poller's
    8 s limit, not instant errors.
  - Decisive: the **laptop reached the same endpoint through the ESP32's NAT at
    0.59/2.97/2.85 s** while the poller failed — versus 61–104 ms an hour
    earlier. The whole uplink was ~30x slower, not just the poller.
  - The 60 s internet probe (12 s timeout) kept logging "reachable" throughout,
    which is exactly why a single red/green cloud is misleading.
  **Diagnostic rule: before blaming the fetch code, compare a laptop request
  through the NAT against the device's own request. If both are slow it is the
  uplink.** And check `rssi` in the `[poll] 60s` line first.
- **Two firmware changes still worth making** (not done yet): back off instead of
  retrying a failing poll at 1 Hz with an 8 s blocking timeout (it burns airtime
  on an already-struggling link), and stop graying `dep`/`arr`/`flight`/`tail` —
  those are static for the whole flight and should survive a transient feed gap
  the way position already does via DR hold.

### Viasat portal API on `wifi.inflight.viasat.com` (all ON the aircraft, ~170 ms)

Derived from the SPA bundle `/static/assets/index-*.js`, not by guessing:

| endpoint | gives |
|---|---|
| `/ac/flight/info` | 21 fields; we parse ~9. Unused: `service_available`, `internet_use_enabled`, `service_disable`, `mode`, `fm_authenticated`, `estimated_arrival`, `take_off_time` |
| `/ac/device/info` | `status` ("transparent"), `status_reason`, our STA `mac`/`ip`, a `pppt` session UUID |
| `/plans/plans.json` | 6 packages with `quota_bytes` / `quota_seconds` |
| `/ac/captcha/enabled` | `captcha_enabled` |
| `/ac/device/connect`, `/ac/captcha/sequence` | **do not probe** — session-mutating / consumes a token |

- **The plan is TIME-limited, not byte-metered:** every package is
  `quota_bytes: null` + `quota_seconds: 86400`. `netcore.c` justified probe
  design "on the metered link" — that premise is false, there is no byte budget
  to protect. The ~368 kbit/s ceiling is a rate class ("streaming excluded").
- `/ac/device/info` confirms the whole cabin side appears to Viasat as **one
  device** (our STA MAC, and `user_agent: "aidlink"` from our own probe).
- The poller parses `groundSpeed`, but the live Viasat payload **has no such
  field** — `derive.c` is load-bearing here, not a fallback.
- Don't log the `pppt` UUID or MAC into flog / the public repo.

### Three-state internet status (shipped)

`netcore` stays provider-agnostic: `portal` and `down` come from the generic
captive-portal probe (any non-204 answer = interception; no answer = dead link),
and only `service_off` needs provider knowledge, delivered via
`netcore_service_hint(SVC_YES/NO/UNKNOWN, reason)`. The Viasat mapping lives in
`poller_sources.c` next to the position parsers. **A new provider = one function
there plus one dispatch case; netcore is untouched.** Hints expire after 120 s so
a stalled source cannot pin a stale verdict, and a payload with no service fields
must yield UNKNOWN, never NO — asserting an outage from silence would report a
dead uplink while the internet is fine.

## 2026-09-04 — DNS cache + AP airtime tuning; and where the uplink ceiling actually is

Second in-flight round on the same Viasat leg (VTBS→NWWW). Build 68 flashed over
the cable, hash verified, no RST tap. **6 AP clients stayed associated across the
Wi-Fi change** — that was the risk and it did not bite.

- **The uplink ceiling is the provider's shaper, not us.** Three 400 KB transfers
  gave 45,572 / 46,166 / 47,038 B/s — within 3%. That consistency is a token
  bucket at **~368 kbit/s**, and no device-side change raises it. Corollary worth
  remembering: **the 8.8% ICMP loss is ICMP-specific deprioritization, not data
  loss.** At 936 ms RTT genuine 8.8% loss caps TCP near 6 kB/s; we measured a
  rock-steady 46. Don't chase "packet loss" that TCP plainly isn't seeing.
- **The Viasat position endpoint is ON THE AIRCRAFT, not across the satellite.**
  `wifi.inflight.viasat.com` answers in **61–104 ms** vs ~940 ms satellite RTT. So
  the 1 Hz `poll_ms` costs *nothing* on the metered link. I nearly recommended
  slowing it to reclaim ~3.5% of bandwidth — the estimate was pure fiction built
  on the assumption it crossed the satellite. **Measure the endpoint, don't assume
  the topology.**
- **`build/sdkconfig` is the authoritative config, not `firmware-idf/sdkconfig`.**
  `idf.py` prints `-- Project sdkconfig file .../build/sdkconfig`. Editing the
  top-level one changes nothing. Worse: an incremental `idf.py build` will **not**
  regenerate `build/config/sdkconfig.h` when sdkconfig changes, so it silently
  compiles the OLD values — and even `idf.py reconfigure` won't help if you edited
  the wrong file. Always verify the value actually landed:
  `grep '#define CONFIG_ESP_WIFI_RX_BA_WIN ' build/config/sdkconfig.h`.
- **A safety flag silently undid the PSRAM Wi-Fi tuning.** `ESP_WIFI_RX_BA_WIN`
  defaults to 16 only `if (SPIRAM_TRY_ALLOCATE_WIFI_LWIP && !SPIRAM_IGNORE_NOTFOUND)`.
  We set `SPIRAM_IGNORE_NOTFOUND=y` deliberately as a boot guardrail, which drops
  the window back to **6** — Espressif's help calls 16 the recommended minimum in
  exactly this configuration. Now pinned explicitly (16/16, dynamic RX/TX 64) in
  `sdkconfig.defaults.esp32s3` only; `STATIC_RX_BUFFER_NUM` stays 10 because those
  buffers must come from internal DMA SRAM, and the non-PSRAM ESP32 shares nothing.
- **The DNS cache's value is determinism, not raw latency — the garden's resolver
  already caches.** Pre-flash, repeats of one name gave 11/32/16 ms (upstream
  cache hit) but another gave 1353/360/770 ms. Post-flash every repeat is
  **2–6 ms** after a 594–745 ms first lookup. So the win is that repeats become
  local, deterministic, **survive a satellite dropout**, and are **shared across all
  clients** — each phone/tablet otherwise keeps its own private OS cache. Do not
  claim "saves 660 ms per repeat"; often upstream would have served it in ~20 ms.
- **Cache correctness choices worth keeping:** never cache SERVFAIL/REFUSED (on
  this link they are a transient drop, and pinning one turns a blip into an
  outage); never cache TC=1; NXDOMAIN/NODATA cached at the 5 s floor since they
  carry no answer TTL; TTL clamped to [5,300] s; compression pointers are stepped
  over and **never followed** when walking RRs. `dnshit`/`dnsmiss` on `/status`.
- **macOS will not rejoin the AP while the USB-NCM link works** — `en0` sat
  `status: inactive` for 5+ min after the reboot. Don't read that as "the AP is
  broken": check `clients` on `/status` (that field is `esp_wifi_ap_get_sta_list()`,
  a live association count, not a DHCP lease). Toggling Wi-Fi needs sudo, so the
  Wi-Fi-path measurement needs the human (menu-bar toggle avoids sudo entirely).
- **The AP tuning measurably worked — this is the win for the cable-less clients.**
  Same 6784 B local page, same method, before vs after (8 runs vs 6):

  | | TCP connect | total | throughput |
  |---|---|---|---|
  | before | 23.6–111.0 ms | 0.124–0.635 s | 10.7–54.6 kB/s |
  | after  | **4.5–21.7 ms** | **0.035–0.206 s** | **33.0–194.7 kB/s** |

  Median throughput ~26.6 → ~99 kB/s (**≈3.7×**); worst case 10.7 → 33.0 kB/s;
  worst TCP handshake 111 → 22 ms (**5×**, and an earlier session saw 2.96 s
  handshakes). 40-packet RTT 4.16/29.47/111.67 → 3.23/**21.39**/93.76 ms. All 8
  runs beat the pre-change median, so this is not a lucky sample — though the
  cabin channel is shared and noisy, so treat absolute numbers as indicative and
  trust the connect-time column, which is least sensitive to that noise.
- **7 devices share ONE shaped session.** All AP clients are NATed through a single
  Viasat allowance with no QoS anywhere in lwIP, so one device doing a photo sync
  starves the rest. On this link, *contention beats tuning* — the biggest available
  win is fewer active clients, not firmware.

## 2026-09-03 — Uplink tuning: NAPT/dnsfwd timers were LAN-shaped; measured in flight on Viasat

Live measurement from the cabin (Board 3 as AP, Viasat uplink, MacBook on Wi-Fi),
then four fixes. **Flashed in flight over the cable (build 66) and verified on the
wire** — results at the end of this entry.

**Verified after flashing (2026-09-03, ESP32-S3 d0:cf:13:32:2f:48, hash verified,
came back with no RST tap):**
- AAAA now answered on-device: **2444 ms mean → 2 ms**, 6/6 instead of 5/6, and the
  11 s stall class is gone. `TYPE65` likewise 2–3 ms. Both return NOERROR with
  ANSWER: 0 (NODATA), and **A is unchanged at 666 vs 671 ms — no regression.**
- **UDP NAT mappings now survive an 8 s idle gap** (3/3 NTP servers, second reply
  from the same source port after idling) — under the old 2 s timeout that reply
  was dropped. Direct on-the-wire proof the timer override is live.
- ⚠️ **Attribution trap:** the post-flash run happened over the USB-C cable
  (`route get default` → `en12`), so the local-RTT gain (38.1 → 3.2 ms mean, max
  100.7 → 10.4 ms) and most of the throughput gain (8.3 → 47.2 kB/s, and the 1 MB
  download finally *completing*) are **the cable, not the firmware**. Satellite RTT
  actually got *worse* in the same window (790 → 1133 ms), which is the clearest
  reminder that only same-path, same-window comparisons mean anything here.
- **Test-tooling trap:** macOS `dig` does not know the `HTTPS` type keyword — `dig
  HTTPS name` silently queries a bogus *name* at type A (NXDOMAIN, full satellite
  RTT) and looks like the local-answer fix failing. Use **`dig TYPE65`**.

- **esp-lwip's NAPT timers are wrong for a satellite uplink and have no Kconfig.**
  `IP_NAPT_TIMEOUT_MS_UDP` is **2 s** — *shorter than one round trip* on this link
  (measured 682–989 ms, mean 790 ms, and the poller already allows 12 s). Every UDP
  flow with a >2 s gap loses its translation: QUIC/HTTP3 on UDP 443 (Chrome/Safari
  default) and any VPN keepalive (WireGuard 25 s) get torn down continuously.
  RFC 4787 REQ-5 wants ≥2 min. ICMP is also 2 s, which silently caps how slow a
  ping may be before the reply is discarded as unmapped — misleading precisely
  when you're diagnosing a slow link. TCP is the other extreme: **30 min** idle
  retention in a 512-entry table, so dead entries can exhaust it and then NEW
  connections fail while ping and established flows keep working.
  All three are `#ifndef`-guarded → override with `idf_build_set_property(
  COMPILE_DEFINITIONS ...)` in the root CMakeLists **before `project()`**, then
  verify they actually landed: `grep IP_NAPT build/compile_commands.json`.
- **NAPT cannot translate ICMP errors, so client PMTUD is structurally broken.**
  `ip4_napt.c` handles *only* `ICMP_ECHO`/`ICMP_ER` keyed by echo id. An inbound
  type 3 code 4 "fragmentation needed" carries an embedded TCP header, matches no
  entry, and is dropped. **And lowering the STA MTU does not fix it**: `ip4.c`
  runs `ip_napt_forward()` (which rewrites the source) *before* the MTU check, so
  the `icmp_dest_unreach(ICMP_DUR_FRAG)` it generates is addressed to the
  post-NAT source — our own STA IP — and never reaches the client. DHCP option 26
  is the only sound lever, and IDF **hardcodes it to 1500** (`dhcpserver.c`:
  `*optptr++ = 0x05; *optptr++ = 0xdc;`) with no Kconfig and no runtime setter.
  Measured verdict: **not our bottleneck** — 744 KB of a 1 MB download did get
  through, so full-size packets flow. A real black hole stalls at ~10–20 KB.
- **`dnsfwd`'s 3 s slot timeout was destroying valid replies.** Observed live: an
  AAAA probe took **11054 ms** and returned nothing, while A queries in the same
  minute averaged 671 ms. The reply arrives, the slot has already been freed, so
  `p->used` is false and it's dropped — the client then retries and pays another
  full satellite round trip for an answer we'd already received. Now
  `DNSFWD_TIMEOUT_MS 9000`, and slots 16→32 (a stub fans out A+AAAA+HTTPS per
  hostname, so 16 evicted queries that were still in flight).
- **AAAA is guaranteed-useless traffic on this device and worth answering locally.**
  `CONFIG_LWIP_IPV6_FORWARD` is off and we send no RA, so a client only ever gets a
  link-local address and no v6 default route — any AAAA we relay names an address
  it provably cannot reach. Measured cost of that waste: AAAA mean **2444 ms**,
  HTTPS/SVCB (type 65) mean **963 ms**, per hostname. Now answered locally as
  **NODATA** (NOERROR + ANCOUNT=0), never NXDOMAIN — NXDOMAIN denies the whole
  name and can poison the A lookup for the same host.
- **The local Wi-Fi hop is the unexpected co-bottleneck, and it is NOT what we
  fixed.** Same 6784-byte local portal page, six consecutive fetches: 78 ms /
  198 ms / 322 ms / 396 ms / 3.29 s / 6.07 s, i.e. 87 kB/s best down to 1.1 kB/s.
  `time_connect` to 172.20.1.1 alone ranged 6.7 ms → **2.96 s** (≈ SYN retransmit
  backoff), while ICMP showed **0.0% loss over 60 packets** at 4.1/25.7/159.2 ms.
  0% ping loss with multi-second local TCP handshakes = airtime starvation /
  burst drops, not link loss. Cause is structural: one radio, `ap.ap.channel = 0`
  follows the STA onto whatever congested cabin channel Viasat uses, and every
  client byte crosses that channel twice. Candidate fixes (`esp_wifi_config_11b_rate(
  WIFI_IF_AP, true)` for ~6× cheaper broadcast airtime; Wi-Fi buffer/BA-window
  raises now that PSRAM is on) were **deliberately not attempted in flight** —
  they change what the AP advertises to associating clients.
- **There is no OTA path on this hardware — do not touch `/dfu` without a cable.**
  `partitions.csv` has a single `factory` app partition (no `ota_0`/`ota_1`) and
  `web.c` has no upload endpoint. `/dfu` only sets `RTC_CNTL_FORCE_DOWNLOAD_BOOT`
  and reboots into the ROM downloader — the unit leaves the network and needs
  `idf.py flash` over USB plus a physical RST. In the air with no cable that is an
  unrecoverable lockout, so uplink fixes cannot be validated until back on ground.
- **Benchmark caveat:** the cabin uplink comes and goes, so absolute throughput
  numbers (8.3 kB/s over 90 s, 4.0 kB/s over 40 s) are not steady-state. Trust the
  *relative* same-window comparisons (AAAA vs A vs HTTPS) and the local-hop
  numbers, which don't involve the satellite at all.

## 2026-08-16 — Ownship heading twitch = per-axis coordinate quantization; derive.c reworked

- **Live capture in flight (F-ONEA ACI410 NWWW→NZAA, Board 3 on the cable):**
  /status `trk` stepped −10.6/+19.8/+8.6° between 2-s samples and `gs` flapped
  388–551 kt in steady flight. Root cause: the feed serves **lat with 4 decimals
  but lon with only 3** (174.447 / .451 / .456…) — 0.001° of lon at 35S is
  **91 m**, ~40 % of the distance flown per second, so adjacent-sample bearings
  are quantization noise and the α=0.35 EMA passed half of it through to the
  EFB (ADBP TRK/THDG/GS come straight from the derived values, and the DR
  extrapolation steers along the same track).
- **The axes are NOT symmetric and the error is course-dependent:** coarse lon
  is along-track (harmless) on an eastbound leg but cross-track (dominant) on a
  northbound one. Any fixed baseline length is wrong somewhere.
- **derive.c reworked (TDD, all 17 host suites pass):** per-axis decimal-
  precision estimate from the incoming values (sliding max over 30-sample
  blocks, so a trailing-zero value like 171.310 can't shrink it); 96-entry ring
  of distinct fixes; track taken over the NEWEST baseline whose projected
  bearing error (per-axis quanta rotated into the cross-track direction) is
  ≤ 0.7°; GS over the **polyline** distance to the oldest entry within 90 s —
  a straight chord under-measured a 90° turn by ~10 % (425 vs 470 kt in the
  turn test); weighted vector-EMA fallback while nothing clean exists yet.
  Sim results: 1 Hz q4 track steps 0.94°→0.18°/s; the live q3-lon case 0.17°/s
  with GS within ±5 kt.
- **Teleport rejection now RESETS the ring** instead of advancing the baseline:
  the same capture showed a genuine feed glitch (lat …2948 → **…2695** → …3018,
  a 1.5 NM out-and-back) — with a ring, a glitch entry must not be allowed to
  donate an endpoint to any later bearing.
- Watch it on grep: `dec_places()` tolerance is 1e-3 after scaling — a parsed
  "171.306" double is integer×10³ to ~1e-8, an unquantized GNSS double walks to
  the 6-decimal cap (≈0.11 m quantum → newest baseline always qualifies).

## 2026-08-13 — DR hold + persistent flash log shipped; bench-verified end-to-end

Implemented the spec below same-day (commits pending): `dr_hold_ms` (default 5 min),
tri-state fresh/DR/NCD in ADBP, OnEvent fallback cadence, subscribe/transition
logging, `pos_state` on /status, and `flog` — a 512 KB flash partition persisting
every logln line with uptime+UTC timestamps, downloadable at `/flog`.

- **DR hold verified live on Board 3** with a custom feed (Mac serving a moving
  Viasat-shape JSON at 480 kt, then killed): 62 s into the stall, getParameters
  still returned `validity="1"` with the position *extrapolated* +8.3 NM along
  track (exactly 62 s at 480 kt), FOM 132.0 (=8+2·age_s), HDOP 1.4, GNSS_AVAIL 1;
  `/log` showed `[adbp] pos fresh -> dr (fix age 30s)` right at the stale gate.
  Old firmware returned bare `validity="2"` in the same state.
- **The custom-source trick is the right DR test rig** — the emulator can't test
  DR (it refreshes `last_fix_ms` forever and sets `fixed`, which disables
  dead-reckoning). `srcType=2` + a Python server on the NCM Mac (172.20.1.2)
  exercises the real poller/derive/arbiter path, and killing the server IS the
  in-flight stall. Viasat JSON has no track: serve *moving* positions and let
  `derive` produce track/GS.
- **Partition-table change over /dfu works**: add `0x8000 partition-table.bin`
  to the same `--no-stub --before no_reset` write as the app. NVS at 0x9000 is
  untouched (pt is 0xC00 bytes at 0x8000). New table: `flog, data, 0x40,
  0x310000, 0x80000` — sized to fit the 4 MB T3-S3 too. flog headers require
  magic AND rsv==0 so pre-existing flash garbage can't fake a sector.
- **The post-dfu-flash soft-reboot USB wedge is a coin flip, not "first reset
  only"**: this session the first /save reboot after flashing re-enumerated in
  2 s, the *third* one wedged (stale NCM iface `active`, Espressif still in
  `ioreg`, 100 % ping loss, no usbmodem port). The 2026-07-15 "deterministic
  first soft reset" model is wrong. Assume ANY soft reset after a dfu-flash
  session may wedge until a physical RST/replug has happened.
  **Post-mortem via flog (the new log's first real catch):** the app ran
  perfectly through the whole 23-min wedge (heartbeats + poll summaries, heap
  stable) — USB-only failure, as theorised in 2026-07-15. The cable replug left
  **no boot marker** = with a battery on the JST it truly is not a power cycle,
  which is why the wedge survived it; only the RST tap (logged as
  `reset=poweron` — an EN pulse reads as poweron, not `ext`) cleared it.
- **Pin ADBP bench probes with `nc -s 172.20.1.2`** — nc has no `--interface`,
  and the Mac's Wi-Fi overlaps 172.20.0.0/20; binding the source IP forces en12.
- The zsh no-word-split trap (`for t in "..."; clang $t`) bit again in the
  host-test runner — `${=t}`, always. All 18 host suites pass.
- `/save` h_save bugs fixed while there: presence-parsed `logEnable` turned
  logging OFF on every save (field never rendered — /logtoggle owns it now),
  and `gpsEn`/`gpsPref` were silently disabled whenever the GNSS card was
  hidden (now gated on a hidden `gpsCard` marker rendered with the card).

## 2026-08-13 — Jepp ownship disappearance = the NCD cliff at stale_ms (analysis only)

Root-caused the in-flight "ownship vanishes then re-appears" report (distinct from
the 2026-07-24 socket-pool incident — here :80 stays healthy). Full design in
`docs/superpowers/specs/2026-08-13-adbp-dr-hold-spec.md`. Key facts:

- **A fetch failure never invalidates anything** (`poll_once` returns early, no
  failure counter); the only clock is `age = now - last_fix_ms` vs `stale_ms`. Below
  it, ADBP frames are validity=1 and **already dead-reckoned** along last track/GS
  (`adbp.c:55-58`, `adbp_frame.c:31-41`, dt clamped to stale_ms). At it, a cliff:
  coarse+fine GPSLAT/GPSLONG all go `validity="2"` bare-NCD and `GNSS_AVAIL=0` —
  the EFB has no fix and drops the symbol.
- **OnEvent subscriptions are worse: silence from the FIRST failed poll** — `due`
  needs `last_fix_ms` to change (`adbp.c:176,182`), so stale NCD frames are never
  even sent. We don't know which mode FliteDeck negotiates; log it on subscribe.
- **NVS beats code defaults on field units:** the in-service Board 3 still runs
  `stale_ms=15000, poll_ms=4000` (read live via the portal) although the code
  defaults moved to 30000/1000 — so ~4 missed polls ≈ two 8 s HTTP timeouts kill
  the position. Always read the portal, never trust `config.c`, when reasoning
  about a deployed unit's timing.
- **Failures cluster by construction:** every http error closes the keep-alive
  socket (`poller.c:206-207`), so the next poll pays a full TLS handshake over an
  already-lossy link. One radio fade easily spans the whole 15 s window.
- `stale_ms` conflates "fix trust window" (arbiter → GNSS fallback) with "when ADBP
  stops reporting". Zero-code mitigation: raising Stale→NCD in the portal extends
  the DR window (position keeps moving, periodic subs only). Proper fix per spec:
  separate `dr_hold_ms` with degraded FOM/HDOP + OnEvent fallback cadence + honest
  NCD after the cap.
- `service_avail` in `pos_state_t` is write-only dead state; `getParameters`
  `errorcode` means "unknown param", not staleness — no protocol signal of feed
  health exists besides per-parameter validity.

## 2026-08-13 — Post-flight log forensics are impossible: the log is RAM-only

- Asked "can we analyse last flight's logs?" on Board 3 after a flight. Answer: **no,
  by design**. `log.c` is a 90-line × 200-char RAM ring; the partition table is only
  `nvs / phy_init / factory` — no SPIFFS/LittleFS/SD, nothing position- or log-shaped
  ever touches flash. A power cycle erases everything; even continuously powered, 90
  lines of 60-s heartbeats cover barely ~1.5 h and hold no position history.
- **Uptime can be read straight off `/log`**: the display heartbeat fires every 600
  ticks at 100 ms/tick, so `last hb tick × 0.1 s` = uptime (tick 12600 = 21 min).
  Useful because there is no `/hw` endpoint — the hardware card is inline in the
  portal page, and `/status` carries no uptime field.
- Board identity over the cable, reconfirmed: USB descriptor is the anonymous TinyUSB
  NCM `303A:4000` serial `123456` for *every* board — only the NCM MAC discriminates
  (host `en12` ether / ARP of 172.20.1.1 in the `d0:cf:13:32:2f:4x` block = Board 3).
- If flight logging is ever wanted: Board 3 has ~12.9 MB of the 16 MB flash unused
  (app slot 3 MB + nvs 24 KB); a data partition logging 1 Hz fixes costs ~2 MB per
  10-h flight. Requires a partition-table reflash, which per the 2026-07-25 rules
  must NOT be done via the app-slot-only `/dfu` ritual without care for NVS at 0x9000.

Identified a new module from photos + the u-blox data sheet, then **confirmed it live
over its own CP2102** (jumpers on A, read-only UBX polls). Full write-up in
**`GPS-HARDWARE.md`** — only the non-obvious bits here.

- **Board 2 cannot be flashed over native USB — use the CH343 port.** `/dfu` returns
  200 and the app stops, but the ROM downloader **never enumerates a CDC port** on
  macOS; the stale `303A:4000` NCM descriptor just sits in the USB tree. Waited 25 s
  and 60 s, twice. `tools/flash-aid.sh` says "Boards 3 / 4 / 5" in its header for
  exactly this reason — those are native-USB-only. Board 2 has a **second USB port
  with a WCH CH343** (`1A86:55D3` → `/dev/cu.usbmodem5AE60430151`); over that,
  esptool auto-reset worked first try, **no BOOT+RST needed**, and the stub is fine
  (the `--no-stub` rule only applies to the USB-OTG ROM path). 1.7 MB in 22 s.
  Recovery from the failed `/dfu` is a cable replug — nothing was written either time.
- **GNSS data path confirmed end-to-end on Board 2.** After adding
  `.gps_rx=16 .gps_tx=12 .gps_pps=21` to `PROF_S3_DEVKIT`: `gps: listening on UART1
  rx=16 tx=12 pps=21 @ 9600 baud`, and `/status` reports `gps.present=true` with
  **`csum_err=0`**. That single pair of facts validates jumper position B, TX/RX
  orientation, and baud in one shot — `present` can only go true on a checksum-valid
  sentence, so a swapped pair or wrong jumper would have left it false.
- **`gps.present=true` with `sats_view=0` is the useful diagnostic split**: the serial
  side is proven good and the failure is isolated to RF. `age_out()`'s "no valid
  sentence" warning is NOT a substitute — it only fires if the receiver *was* present
  and went quiet, so silence at boot is ambiguous.
- **The fleet now has two receivers with *incompatible* config protocols.** This one
  is a genuine **MAX-M8Q-0-10** — `MON-VER` gives `ROM CORE 3.01 (107888)`,
  `hwVersion=00080000`, `PROTVER=18.00` — so legacy **`UBX-CFG-NAV5`** is the right
  way to set `dynModel`, verified by CFG-NAV5 answering a poll. The 2026-07-25 bench
  part is M10-class silicon (`000A0000` / `34.10`) and needs **`CFG-VALSET`**. Any
  config code must branch on `UBX-MON-VER`, never assume.
- **`hwVersion` is the counterfeit test.** `00080000` = real M8, `000A0000` = M10
  silicon wearing an M8 silkscreen. Cheaper and more definitive than any other check.
- **Board 1 and this HAT both enumerate as CP2102 `10C4:EA60` serial `0001`**, i.e.
  both grab `/dev/cu.usbserial-0001`. The port name cannot tell them apart — read the
  stream (NMEA vs. ESP-IDF log) before assuming which one you are talking to.
- **Live defaults matched the data sheet exactly**: `dynModel=0` portable, 1.00 Hz,
  GPS+SBAS+QZSS+GLONASS on, Galileo/BeiDou/IMES off. Nothing to guess at.
- **`trkChHw` reports 32 tracking channels, not 72.** The "72-channel" figure is
  u-blox's acquisition engine; it never appears in `MON-*`.
- **Zero satellites indoors *and* outdoors**, `GPGSV`/`GLGSV` = `1,1,00` throughout.
- **A frozen `agcCnt` is the "RF path is open" signature.** AGC read exactly **5928**
  on 12 consecutive samples, unchanged after carrying the antenna outside — spread
  **zero**. Meanwhile `noisePerMS` and `jamInd` dithered, proving `MON-HW` was live.
  A working AGC loop always moves by tens of counts; a frozen one means nothing is
  coupling into `RF_IN`. **This is a distinct third signature**, alongside the
  11 %-pinned *low* AGC (under-volted antenna, 2026-07-25) and genuinely high AGC
  (weak sky view). Read the *variance*, not just the value.
- **Identical AGC across two different antennas means the antenna is not the
  variable.** The known-good 2026-07-25 patch and a small pigtail antenna both gave
  `agcCnt=5928` to the count. Swapping antennas changes the load at `RF_IN`, so an
  adaptive AGC must move — a *bit-identical* reading exonerates the antenna and points
  at either missing bias (both antennas active, carrier supplies none — MAX-M8Q has no
  `V_ANT`) or a broken RF path on the carrier. **The discriminator is DC volts on the
  u.FL centre with the antenna off**, not more sky time. `NAV-SAT` was still
  `cno=0 qual=1` (blind search, zero energy) throughout.
- **⚠️ `aStatus=OK` does NOT mean an antenna is connected.** `CFG-ANT` here is
  `flags=0x001b`: `svcs` on, `scd` (short detect) on, **`ocd` (open detect) OFF**.
  With open detection disabled the receiver physically cannot report a missing
  antenna — `OK` means only "no short". I briefly mis-read `OK` as proof that bias
  and antenna were fine; it proves neither. Always poll `CFG-ANT` before trusting
  `aStatus`.
- **ROM part ⇒ config is volatile.** No flash; settings live only in `V_BCKP`-backed
  RAM and no backup cell is visible on the carrier. Baud, nav rate, dynModel and
  constellation selection must be **re-sent on every boot**. `gps.c` is currently
  receive-only, so the module runs power-on defaults today.
- **Galileo is disabled by default** on M8 — `UBX-CFG-GNSS` turns it on. This is the
  cheapest accuracy win available here, because **SBAS is worthless in the South
  Pacific** (WAAS/EGNOS/MSAS/GAGAN all cover elsewhere).
- **L1 single-band, 2.5 m CEP, 10 Hz concurrent / 18 Hz single-GNSS.** Operational
  limits ≤4 g / 50,000 m / 500 m/s — a jet at FL410 and 300 m/s is well inside.
  9600 baud, not the chip, is what caps the practical update rate at ~2 Hz.
- **Supply is 2.7–3.6 V, not 1.65 V** — the 1.65 V figure in the M8 family belongs to
  the MAX-M8C. Easy spec-sheet trap.
- **MAX-M8Q has `LNA_EN` but no `V_ANT`.** Active-antenna bias is MAX-M8**W** only, so
  any bias must come from the carrier board — relevant given the 3.0 V antenna floor
  that bit us on 2026-07-25.
- **Carrier gotcha:** the A/B/C jumpers shipped on **A = USB ~ GNSS**, which leaves the
  on-board CP2102 driving the module's RXD. Pull both before wiring an ESP32 TX to it.

## 2026-07-25 — Wired GNSS on Board 3: pins, a counterfeit M10, and a 3.0 V antenna floor

Bench-validated a serial GNSS module on the T-Display-S3 (Board 3) with a
throwaway probe app. All three signals confirmed working; AIDlink itself was not
modified.

- **Only two GPIOs are free on *every* board in the fleet: 16 and 12.** The
  T-Display-S3 header exposes {1,2,3,10,11,12,13,16,17,18,21,43,44}; intersecting
  that against the T3-S3 leaves just those two. On the T3-S3, **17 = OLED SCL**,
  **18 = OLED SDA**, **21 = QWIIC SCL *and* LoRa DIO3**, **10 = QWIIC SDA + LoRa
  DIO4**, 2/11/13/14 = microSD, 43/44 = UART0 console (`CONFIG_ESP_CONSOLE_UART_DEFAULT=y`).
  Final wiring: **RX=16, TX=12, PPS=21 (Board 3 only)**. GPIO12 is safe on S3 —
  the "GPIO12 is a strapping pin" trap is original-ESP32 only (S3 straps are 0/3/45/46).
- **Keep TX.** It is what allows `UBX-CFG-VALSET` to set the airborne dynamic
  model; the default *portable* model caps at 12 000 m / 310 m/s, and FL390 ≈
  11 887 m sits right on that ceiling.
- **The "NEO-M8N" is a counterfeit — the silicon is M10.** Can reads
  `NEO-M8N-0-10`, but MON-VER reports `hwVersion=000A0000`, `PROTVER=34.10`,
  `ROM SPG 5.10`. A real M8N is `00080000` / PROTVER 18–20.x / `ROM CORE 3.01`.
  Consequence: **legacy `UBX-CFG-NAV5` does not exist on this part** — all config
  must go through `CFG-VALSET` (`CFG-NAVSPG-DYNMODEL` = `0x20110021`, value 8).
  Firmware runs from mask ROM, so there is nothing to update.
- **The active antenna has a 3.0 V floor, and the board is 5 V-input.** With VCC
  on the 3V3 rail the module's AMS1117 (1.1 V dropout) produced ~2.2 V — under the
  antenna's rated minimum. Signature: **AGC pinned at 11 %** (`UBX-MON-RF`), not
  the high AGC a *disconnected* antenna gives. On 5 V, AGC recovered to 30–45 %.
  ⚠️ The header 5 V pin is VBUS, so **the GNSS dies on JST battery** — needs a
  boost, a low-dropout module, or a passive antenna before this becomes a
  position source.
- **`UBX-MON-RF` is the diagnostic that matters.** AGC direction disambiguates:
  *low* AGC = too much input power (interference / unstable LNA), *high* AGC =
  no signal reaching the front end. NMEA alone cannot tell these apart.
- **Patch antennas are directional — the labelled face is the BASE.** Label-up
  cost ~12–15 dB (best SNR 28 dB-Hz, 1–2 sats). Flipped label-down onto a metal
  tray as a ground plane, under open sky: **6 sats, HDOP 1.30, 3D fix**.
- **PPS gotcha, self-inflicted:** `gpio_config()` with `.intr_type =
  GPIO_INTR_DISABLE` followed by `gpio_set_intr_type()` **leaves the interrupt
  disabled** — the ISR registers but never fires, which looks exactly like a dead
  wire. Needs an explicit `gpio_intr_enable()`. Always cross-check an edge count
  with a polling task before blaming hardware. Once fixed: `isr=10 poll=10 in 10 s`,
  interval 1.000 s, ~10 % duty (100 ms pulse) — the u-blox default.
- **Board-3-only PPS:** the eventual driver must carry the pins in the per-model
  `board_t` profile (`.gps_pps_gpio = -1` on `PROF_T3S3`), so Boards 2/4/5 stay
  unaffected and GNSS stays optional.
- **The `/dfu` "wedge" was never `/dfu` — it is the esptool flasher stub.**
  After `/dfu` the ROM enumerates on **USB-OTG** (`USB mode: USB-OTG` in
  esptool's banner), NOT USB-Serial-JTAG. The stub uploads, prints
  `Stub running...`, and the port then goes permanently silent; only a cable
  replug recovers it. All five wedges in this session followed those two lines.
  **`--no-stub` from the FIRST connection makes the cycle reliably hands-off**
  — passing it as a retry never helps, because by then the session is poisoned.
  Second rule: `--before no_reset`, since after `/dfu` the chip is already in the
  downloader and resetting it again is another way to lose it.
  `tools/flash-aid.sh` encodes both.
- **Flash ritual, reconfirmed:** `/dfu` sets `FORCE_DOWNLOAD_BOOT`, which survives
  a soft reset — a flashed app may NOT run until an **RST tap or power cycle**,
  because esptool's "hard reset" over USB is a USB-level reset, not an EN pulse.
- **Probes must loop forever.** A one-shot diagnostic finishes its ~35 s run
  during the round-trip between flashing and opening the console, and reads as a
  dead board.

## 2026-07-25 — GNSS shipped as a selectable source: what the hardware taught

Implementation of the spec above. Seven feature commits on
`feat/gps-position-source`. Bugs worth remembering, all found by running rather
than by reading:

- **"3D fix, 0 satellites used, HDOP 99.99"** — self-contradictory state seen
  live. Cause: per-constellation GSA fix dimensions were kept indefinitely, so a
  constellation that stopped reporting left a stale 3D that won the max().
  **GGA's quality field is the single authoritative answer** to "is there a fix";
  GSA only refines it to 2D/3D. Gate on GGA, always.
- **GSA `systemId` is field 18, not 17.** Field 17 is VDOP, which parses as a
  perfectly believable constellation id — a silent wrong answer, not a crash.
- **A trailing empty GSA erased a good 3D fix** reported moments earlier by
  another constellation. Receivers emit one GSA per constellation every cycle.
- **PPS "locked" was a lie.** `pps_interval_us` is the last measured gap; when
  the fix drops the receiver stops pulsing and the value goes stale. An
  18-second interval displayed as locked. Require 0.9–1.1 s.
- **LVGL paints children in creation order.** An opaque overlay built early was
  drawn *under* the route widgets built later. `lv_obj_move_foreground()` at
  show time, rather than reshuffling the build.
- **`gpio_config()` with `GPIO_INTR_DISABLE` then `gpio_set_intr_type()` leaves
  the interrupt off.** The ISR registers and never fires — indistinguishable
  from a dead wire. Cross-check any edge count with a polling task before
  blaming hardware.
- **`LV_SYMBOL_GPS` already exists** in the Montserrat symbol font; the planned
  font-generation step was unnecessary.
- **`layout_oled.c` needed no change**: its indicator is driven by
  `pos_fix_seq()`, which the arbiter bumps for any source.

### Verified on Board 3

3D fix through the new parser (6 sats, HDOP 1.30), PPS at 1.000012 s, per-
constellation solving counts, both portal cards, the three header tiles, the
`gps` object in `/status`, and the two bug fixes above. All 10 host suites pass.

### NOT yet verified (needs sky the bench does not have)

The **green** tile/icon state, an actual **switch to GPS as the live source**,
**fallback** in either direction, and the **no-GNSS board** regression check on
Board 2/4. The code paths are host-tested but have not been exercised end to end
against a live fix.

## 2026-07-24 — In-flight position dropout in Jepp = lwIP socket-pool exhaustion

Confirmed fix on the in-service T-Display-S3 (Board 3), in flight.

- **Symptom:** aircraft position intermittently vanished from Jeppesen FliteDeck;
  a reboot restored it for ~15 min, then it dropped again.
- **Diagnostic signature (remember this):** ICMP and NAPT forwarding stayed
  healthy while BOTH socket servers (httpd :80 and ADBP :24000) failed together
  — accept then RST / zero bytes. **Socketful services dying while socketless
  paths survive == socket-pool exhaustion, not CPU or heap.** `/status` showed
  8.4 MB free heap, which ruled out memory outright.
- **Root cause:** `CONFIG_LWIP_MAX_SOCKETS=10`, and esp_http_server's default
  `max_open_sockets=7` is entitled to 7 of the 10 (httpd requires
  `LWIP_MAX_SOCKETS-3 >= max_open_sockets`), leaving 3 for dnsfwd (2 UDP), the
  ADBP listener + one push socket PER subscribed EFB, the poller keep-alive TLS,
  mDNS, and the internet probe. Under load (several Wi-Fi clients + Jepp
  subscribed) the pool exhausted, ADBP's push `socket()` failed, position gone.
  The ~15-min recovery window is just how long the pool takes to refill.
- **Fix (commit 22af8f0):** `LWIP_MAX_SOCKETS` 10→16, cap `hc.max_open_sockets=4`,
  `MAX_SUBS` 6→4, and a `logln` in `connect_push` when `socket()` fails so the
  next occurrence is visible in `/log` instead of inferred.
- **Verified:** an inbound ADBP `getParameters` that RST/0-bytes'd before now
  returns GPSLAT/GPSLON `validity="1"` + `GNSS_AVAIL=1`; position held past the
  15-min mark in flight.

### Process lessons (I got two things wrong first)

- **Don't trust TCP-service probes from a dual-homed Mac.** When the Mac had BOTH
  Wi-Fi (en0) and USB-NCM (en12) on the AID's 172.20.1.0/26, connections to
  172.20.1.1 RST and latency read ~12 ms — pure routing artifact. Single-homing
  (Wi-Fi off) dropped latency to ~2 ms and the RSTs that REMAINED were the real
  ones. Always test the AID from a single path.
- **My probing consumed the scarce sockets** and hastened the dropout — observer
  effect. On a 10-socket pool, hammering :80/:24000 is not free. Verify with ONE
  connection, not a retry loop.
- I initially theorised "2 MB PSRAM / memory pressure" — wrong board (this is the
  8 MB T-Display-S3) and wrong resource (heap was 8.4 MB free). The signature,
  not the guess, is what nailed it.

### Reflashing the in-service T-Display-S3 in flight (worked)

`/dfu` (auth-gated) forces download-boot (`RTC_CNTL_FORCE_DOWNLOAD_BOOT`) and
restarts — but the ROM port re-enumerates, so poll for whatever `/dev/cu.usbmodem*`
appears and flash it IMMEDIATELY with `esptool --before no_reset` (it's already
in download mode; a reset knocks it back to the app). One tight script:
trigger /dfu → watch for the port → flash. Needs the portal (:80) up first, so if
the AID is wedged, the user must power-cycle before /dfu is reachable.

## 2026-07-23 — Board 4 (LilyGO T3-S3) identified: S3FH4R2, quad 2 MB PSRAM

- New unit is a **LilyGO T3-S3 LoRa** board, not another N16R8: **ESP32-S3FH4R2**
  = 4 MB flash **and** 2 MB PSRAM *in-package*, both **quad**, MAC `1c:db:d4:bd:1f:1c`.
- **Getting esptool onto a native-USB board that is running an app:** esptool cannot
  reset it (`--before default_reset`/`usb_reset` → *"No serial data received"*),
  because the app's TinyUSB CDC owns the port. **1200-baud touch works**: open the
  CDC port at 1200 baud with DTR low, close it — the app reboots into the ROM
  downloader and re-enumerates under a *different* device node (`usbmodem101` here,
  not the MAC-derived one). Same trick should unstick any TinyUSB board.
- `ioreg -p IOUSB -w0 -l` is the reliable way to read VID/PID/product string on this
  Mac — `system_profiler SPUSBDataType` returned nothing.
- **AIDlink won't run correctly here yet:** `sdkconfig` has `CONFIG_SPIRAM_MODE_OCT`;
  quad PSRAM fails to init and `CONFIG_SPIRAM_IGNORE_NOTFOUND=y` hides it, so the
  build boots with *zero* PSRAM. Needs `SPIRAM_MODE_QUAD` + a `board.c` entry.
- The LoRa variant (SX1276/SX1262/SX1280/LR1121) is **not** host-detectable — read
  the shield silkscreen or probe the radio's SPI ID register.

### AIDlink running on Board 4 (verified over the cable)

- Boots clean on the quad profile: `Found 2MB PSRAM` / `SPI SRAM memory test OK`,
  TinyUSB NCM enumerates as `Espressif Device` (PID `0x4000`, serial port gone),
  the Mac's `en12` takes **172.20.1.2/26**, ICMP to 172.20.1.1 is 3/3 at 1.9 ms,
  and the portal answers `/` → 302 → `/login` (6784 bytes).
- ⚠️ **`sdkconfig.defaults.<target>` is auto-loaded AFTER `sdkconfig.defaults`.**
  A per-board profile listed in `SDKCONFIG_DEFAULTS` therefore has to override
  everything that file sets, not just the one setting you care about. Missing
  the flash size bootlooped Board 4: bootloader said `SPI Flash Size : 16MB`,
  then `spi_flash: Detected size(4096k) smaller than the size in the binary
  image header(16384k). Probe failed.`
- ⚠️ **Reflashing a bootlooping native-USB board is the hard part.** A chip
  resetting every ~800 ms tears down its own USB-Serial-JTAG endpoint mid-sync,
  so *every* esptool mode fails with "No serial data received" — the
  1200-baud touch included. Read the error text as state:
    * `No serial data received` + **silent** port → chip wedged; only a full
      power cycle recovers it (unplug USB **and** the JST battery — a replug is
      not a power cycle while the battery is attached).
    * `Invalid head of packet (0x1B)` → 0x1B is ESC from ANSI-coloured
      `ESP_LOG`: the chip is alive and *talking*, so `--before default_reset`
      can now drive the strap. This is the state you want; retry immediately.
- ⚠️ The Mac's Wi-Fi sits on `172.20.0.0/20`, which **contains** the AID's
  `172.20.1.0/26`. `route -n get 172.20.1.1` reports en0, but the longer-prefix
  route and the ARP entry both resolve via the cable. Always pin curl with
  `--interface en12` or you may be talking to the wrong network entirely.

### Bring-up probe results (flashed to the board, same day)

- **SSD1306 acks at 0x3C on SDA 18 / SCL 17.** Swapped pins ack nothing; QWIIC
  (10/21) is empty. A dark OLED at rest is normal — the chip powers up with the
  display driver off (`0xAE`) and LilyGO's factory demo never inits it.
- **`CONFIG_SPIRAM_MODE_QUAD` works: `INIT OK, 2048 KB`**, 2045 KB SPI heap +
  377 KB internal. Confirms the two-build-profile plan (octal for Boards 2/3,
  quad for Board 4) rather than one binary.
- ⚠️ **`i2c_master_probe()` hangs forever on this board** — but *only* that call.
  It spins in `i2c_ll_is_bus_busy()` (`esp_driver_i2c/i2c_master.c:543`) with
  **no timeout**, so the task never returns and only `task_wdt` on IDLE0 reveals
  it — the backtrace is identical every 5 s, which is the tell for "blocked in
  one call" vs "looping". The rest of the `i2c_master` driver is fine: a
  follow-up probe drove `esp_lcd_new_panel_io_i2c` → `esp_lcd_new_panel_ssd1306`
  → `esp_lvgl_port` on the same bus with no stall. Avoid `i2c_master_probe`, not
  the driver.
- ⚠️ **`esp_lvgl_port`'s mono transform is polarity-inverted for an SSD1306.**
  `_lvgl_port_transform_monochrome` (`esp_lvgl_port_disp.c:615-619`) writes a
  **set** bit for BLACK pixels and clears it for lit ones; a set bit lights an
  SSD1306 pixel, so a black-background UI renders as a fully white screen with
  unlit text. Fix: `esp_lcd_panel_invert_color(panel, true)` (0xA6/0xA7) right
  after `esp_lcd_panel_init`, which lets the layout keep the colour board's
  dark-background / bright-text convention instead of restyling to black-on-white.
- **Full mono display stack verified on Board 4**: `esp_lvgl_port` with
  `.monochrome = true` + `LV_COLOR_FORMAT_I1` + full 128×64 buffer works at
  `LV_COLOR_DEPTH=16`, so one binary can drive both the ST7789 and the SSD1306.
  Cost measured: **8 KB internal + 20 KB SPI heap** (300→292 KB / 2048→2028 KB).
- ⚠️ **`printf`/stdout never reaches the USB-Serial-JTAG console here** even with
  `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, while `ESP_LOG` does. Use
  **`esp_rom_printf`** for bring-up output (simple specifiers only: %d %u %x %s %c).
- A **bit-banged** GPIO I²C scan is the reliable probe: every wait is bounded, so
  it reports a result instead of wedging. Worth keeping as the pattern for any
  future unknown bus.
- USB-Serial-JTAG console output is dropped while no host is attached — put a
  ~4 s `vTaskDelay` before the boot banner or you never see it.

## 2026-07-21 — "fetch failed" bursts in flight = per-poll TLS handshake starving internal SRAM

Live debug on Board 3 (T-Display-S3) over the USB-NCM link, real ACI740 Viasat feed:
- **Symptom:** position "comes and goes"; `/status` shows `pollmsg:"fetch failed"`
  in long streaks (pollage climbing 24→68 s = 60+ consecutive 1 Hz polls all
  failing) then a clean flip to `age=0` and sustained success. Internet/clients
  fine throughout.
- **Ruled out the network by NAPT identity:** the AID NAPTs client traffic to the
  STA IP, so the Mac's probes and the device's own polls hit the aircraft as the
  *same MAC + same IP* (172.19.128.141). Mac `curl` to the exact Viasat URL: 15/15
  HTTP 200, full 2293 B, **0.07–0.63 s** total (never near the 5 s timeout). Same
  identity, opposite result ⇒ failure is **inside the device's own TLS/HTTP stack**,
  not the endpoint/uplink/captive-portal. This kills the "timeout too short" theory:
  a too-short timeout gives scattered misses vs a slow server, not a 60 s total
  blackout against a sub-second server that then goes fully healthy.
- **Root cause:** `poller.c` did `esp_http_client_init→perform→cleanup` **every
  second** — a fresh mbedTLS handshake pinning ~20 KB of *internal* SRAM per poll
  (PSRAM disabled, `MBEDTLS_DYNAMIC_BUFFER` off, `SSL_IN_CONTENT_LEN=16384`).
  Against steady LVGL+Wi-Fi+NCM+clients pressure, free heap periodically dipped
  below what a handshake needs → `perform` fails in bursts until memory frees.
  `"fetch failed"` also collapsed transport-error / timeout / non-200 into one
  opaque string, hiding which it was.
- **Fix (low-risk, flight-flashable):** (1) one **persistent keep-alive**
  `esp_http_client` reused across polls — one handshake, then cheap GETs; close the
  socket on error so the next poll reconnects clean. (2) `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`.
  (3) **Instrumentation** — `"fetch failed: <esp_err | HTTP nnn>"`, `ESP_LOGW` with
  free heap + largest block, and a new `"heap"` field on `/status`.
- **PSRAM enabled + validated live (now the S3 default):** Board 3 is ESP32-S3**R8**
  (8 MB *octal* PSRAM). Enabled `CONFIG_SPIRAM/MODE_OCT/SPEED_80M` in
  `sdkconfig.defaults.esp32s3` with `SPIRAM_IGNORE_NOTFOUND=y` (boot without PSRAM
  on init failure, not a loop) and FETCH_INSTRUCTIONS/RODATA **off** (2nd-stage
  bootloader stays PSRAM-agnostic → app-time init only → low brick risk even flashed
  in flight). Result on the live unit: `/status` free heap **40 KB → 8.39 MB**,
  poll 12/12 ok, display/Wi-Fi/NCM all up. The earlier "octal boot-loops, bench
  only" caution held in theory but the guardrails made an in-flight flash safe.
- **Internet logo is a FALSE NEGATIVE on this satellite walled-garden.** The reachability
  probe (`netcore.c inet_task`) does a raw TCP handshake to `1.1.1.1:53` / `8.8.8.8:53`.
  Aircalin/Viasat satellite **blocks direct-IP-to-public-DNS on both 53 and 443**, yet
  real internet works: `generate_204` → HTTP 204, `cloudflare.com/cdn-cgi/trace` → 200
  (egress 161.30.203.47, colo SYD). So the port-53 probe reads "no internet" (red cloud)
  when there IS internet. The 2026-07-08 note worried about the *opposite* (port 53
  passing behind an unauthenticated portal = false positive); this network is the mirror
  case. **Fixed** (`netcore.c http204_probe`): probe with an HTTP `generate_204` GET
  (`connectivitycheck.gstatic.com`, port 80, 12 s timeout for satellite RTT, body never
  read), status must be exactly 204. Measured ~0.8 KB/probe; 60 s cadence ≈ 1.2 MB/day —
  actually *less* than the old stuck port-53 probe (~1 MB/day) and correct. Live result:
  log shows `internet reachable`, cloud goes blue. The "content-validated alternative"
  previously declined, now justified by this evidence.
- **Reflash reality (single USB-C):** `/dfu` (auth-gated) forces download-boot, then
  `idf.py flash`, then **one physical RST tap**. Config `/save` also reboots. No
  zero-disruption change exists; both drop the NCM link briefly.

## 2026-07-16 — Orthodromic/vertical ETA rework shipped: 25 chg / 0.9 flips / 20.7 span

- **Implemented the 2026-07-16 spec** (timing fix, vertical schedule +
  descent overlay, closure-semantics bias, route stretch, proximity-gated
  altitude latch) and let its own §9.2 isolation matrix pick the defaults.
  Fleet result (11 A339): 49/3.0/25.6 → **25/0.9/20.7**, final −1.9…0.
- **The matrix vetoed two spec centerpieces**: uniform route stretch (span
  25.6→32.2 alone; front-loaded taper also lost) because the slow DB TAS
  still cancels geometry on BKK/NOU legs — and the applied cruise bias
  (+1.3 min accuracy for 1.7× churn; spec §6.2 rules that out). Both ship
  compiled-but-disabled (`ETAP_STRETCH_APPLY 0`, `ETAP_BIAS_APPLY 0`),
  host-tested in both configurations, ready to flip WITH per-route perf data.
- **The engines' dt now comes from the monotonic clock** (new `mono_ms`
  param on `eta_update`/`etap_update`): whole-second epochs at the 2 Hz
  refresh made every duplicate second count 0.5 s extra → EMAs ran at ~2/3
  of their advertised τ (codex find, verified). True-2700 beat true-1800 in
  replay. A τ host test measures one full time constant end-to-end.
- **Descent overlay**: staged approach overlays the last 60 NM of the 3°
  descent instead of following it — A339 TOD 197→137 NM-to-go (observed
  99–162). Altitude latch engaged on 6/11 flights; the review's proximity
  gate (`dist < 1.5×alt/300 + 60`) kept every mid-cruise ATC descent out.
- `max_range_nm` now generated from Offto's `airplanes.range` (read-only);
  vertical anchors: BKK→NOU FL370+2 steps, CDG→BKK FL350+3, SYD→NOU direct
  FL410 — note the heuristic is calibrated to range=OPERATIONAL distance
  (5500 for A339), not physical range.
- Gotcha: a two-phase τ test is mandatory — seeding the bias EMA at the
  target ratio makes any τ assertion pass vacuously (r0 must differ).
- **Out-of-sample confirmation**: 8 freshly fetched flights (AeroAPI,
  Personal tier = fierce 429s, pace ≥20 s + 65 s backoff; a "-schedule-"
  fa_flight_id means AeroAPI matched a future scheduled instance → cache a
  track-less JSON that poisons re-runs, delete it) — incl. the never-seen
  NWWW→SYD route: 37→17 changes, span 17→13, abserr UNCHANGED 7.7→7.8.
  The worst flight ever recorded (07-15 VTBS→LFPG, 101 changes/7 flips on
  the old firmware) dropped to 24/1.

## 2026-07-16 — A339 ETA replay audit: route progress and bias semantics dominate

- Current firmware replay over all 12 discoverable cached A339 flights (11 in
  `onboard-ip-mock`, one in `offto-ip-mock`) gives mean 49.9 displayed changes,
  3.1 reversals, 25.3-minute error span, and -1.1-minute final error. Freezing
  cruise bias improves stability to 30.1 changes / 1.8 reversals / 21.3 minutes,
  while mean absolute accuracy worsens only about 1.0 minute.
- `eta_made_good_kt()` measures direct-to-destination **closure rate**, not
  along-track GS, yet `eta_profile.c` compares it with predicted cruise GS.
  Airway doglegs therefore masquerade as speed/wind error; observed median
  path-GS minus closure is roughly 6–12 kt on several long-haul tracks.
- Direct-GC progress omits repeatable route geometry: cached track excess is
  +209…343 NM on CDG↔BKK, +49…64 NM NOU→BKK, and +101…177 NM BKK→NOU. Use a
  direction-specific historical route/polyline prior; do not extrapolate the
  already-rejected cumulative in-flight stretch ratio.
- Display refresh is 2 Hz but passes whole-second `time()` to both ETA engines;
  their `dt<=0 ? 0.5` handling counts alternating updates as 1.0+0.5 seconds
  per wall second. The advertised 2700 s bias and 60 s output EMAs therefore
  behave near 1800 s and 40 s respectively, reducing intended stability.
- A339 DB transcription is correct, but semantics are too coarse: 460 kt is a
  single TAS despite Mach/altitude/route variation, while 41000 ft is a service
  ceiling used as cruise/descent-start altitude. More important than editing
  those constants, the model descends to field elevation and then appends a
  separate 60 NM approach, and it never uses live altitude to re-anchor phase.

## 2026-07-15 — ETA corrections shipped: τ2700, 1-min creep, far-out hyst, A20N row

- **Implemented + host-tested** (all 12 tests green, TDD): `ETAP_BIAS_TAU_S`
  600→2700; `condition()` hysteresis +60 s/h-to-go beyond 1 h (cap 420 s)
  and shown-minute creeps ±1 instead of `lround` (the 90 s hyst made every
  change a 2-min jump). Replay before→after: flips 10→3, span 31.8→25.6 min,
  cumulative displayed movement 108→49 min, landing accuracy unchanged.
- **A20N added to the Offto DB itself** (user call: real row, not an alias):
  measured from 2026-07-14 ACI141 — cruise 445 kt (GS 486 − climatology
  tailwind, corrected for that route's measured climatology bias), climb
  7.5/4.8/13.0 min, M0.78, ceiling 39000. `sqlite3` INSERT into `airplanes`
  (dynamic typing stores 7.5 in the "integer" climb column fine — A339 row
  already does), backup at `~/Sites/Swift/Offto/offto.sqlite.backup-20260715`,
  picture blobs left NULL (Offto UI will show no image until one is added).
  Regen `tools/gen_perfdb.py` → 31 aircraft. A20N flight: 173 changes/295-min
  span → 12/6. `perfdb_find()` now also aliases A19N→A319, A21N→A320.
- **τ change moved a test spec**: with τ2700 the bias has deliberately NOT
  converged 20 min into cruise (r≈1.00 — also the seed is contaminated
  high by climb-Mach samples still in the made-good window at cruise entry);
  test now samples r at +40 min. Docs: `docs/eta-estimator.md`.

## 2026-07-15 — ETA replay on 12 real flights: NOT stable; bias EMA + drift diagnosed

- **Replay harness** (`host_test/replay_eta.c` + `tools/extract_track.py`)
  runs the cached AeroAPI flights (`~/.cache/onboard-ip-mock/`) through the
  exact firmware pipeline in virtual time. Verdict on 11 A339 long-hauls:
  26–95 displayed changes/flight, ±20–44 min mid-flight envelope, final
  error −2…+1 min. The synthetic host test's "0 drift" was true but its
  simulated speeds matched the model by construction — real winds don't.
- **The ±10 % cruise bias (τ 600 s) *adds* instability on real flights**:
  freezing it cuts changes 3×; r_ema saturates a clamp 4–18 % of the flight.
  Validated fix: τ→2700 s + far-out hysteresis (+60 s/h beyond 1 h-to-go,
  cap 420 s) + creep shown minute ±1 (never `lround` — stock hyst 90 s > 60 s
  means every change is a 2-min jump). Result: flips 10→3, span 31.8→25.6 min.
- **Two traps tested and rejected**: confidence-ramp bias weight (local
  deviation ≠ remaining-route deviation → span ×2–3; the stock p-scaling is
  *leverage control*, keep it) and cumulative route-stretch (route excess is
  front-loaded → overestimates remaining, span up to 93 min).
- **A20N is missing from perfdb** — Aircalin A320neo flights fall back to the
  reactive estimator (173 changes, 295-min span on 2.4 h; with A320 profile:
  6 changes, 6-min span). Alias neo types or regen from Offto DB.
- **A339 error budget** (reciprocal-pair TAS + wind_probe + oracle replay):
  perf DB cruise 460 kt vs real ~498 (BKK/NOU) / ~465 (CDG) — route-dependent,
  one constant can't fit both; climatology wind error −21…+22 kt with
  day-to-day sign flips (±3–30 min, not correctable offline); CDG legs are
  dominated by +300 NM route-vs-GC geometry (even oracle winds leave 38–46 min
  span). Errors CANCEL pairwise (slow TAS masked headwinds; wind error masks
  geometry) — never tune one parameter judged on one flight; use
  `tools/replay_flight.py` over the whole cached set.
- Full analysis + correction plan:
  `docs/superpowers/specs/2026-07-15-eta-stability-replay.md`.
- Gotcha: zsh does NOT word-split unquoted `$var` — `set -- $f` and
  `clang $FLAGS` silently pass one giant arg; use `${=var}`.

## 2026-07-15 — ETA v4: theoretical profile (FMS-steady) + TOD, perfdb from Offto

- **Design shift:** the made-good estimator can only react; the FMS is steady
  because it *predicts* the whole remaining flight and only nudges the
  prediction. `eta_profile.c` ports Offto's profile (climb 280/380/M·593.7 kt
  with DB minutes, cruise TAS + seasonal ERA5 250 hPa winds via the wind
  triangle per 5°×60° box, descent = ceiling/300 NM integrated over the
  290/250@10k-AGL/180/140 IAS schedule with airport elevation, staged 60 NM
  approach, 0.8× floor). Only adaptive term: made-good vs predicted cruise GS,
  EMA τ600 s, clamped ±10 %, weighted by fraction of cruise flown. Host sim:
  **0 displayed-minute drift across a whole cruise** under ±25 kt oscillation,
  final error 26 s; the winter Pacific jet costs +21 min NOU→NRT (sign-checked).
- **Single DB, two projects:** `tools/gen_perfdb.py` reads the Offto app's
  SQLite **read-only** (`file:…?mode=ro`, canonical:
  `~/Sites/Swift/Offto/Resources/offto.sqlite` — the root-level copy is a
  0-byte stub) and emits committed `perfdb_data.c` (30 aircraft + int8 wind
  grids, ~5 KB). Regenerate manually when Offto's DB changes. Python float
  formatting gotcha: `f"{7.0:g}f"` yields `7f` which is NOT a C literal —
  guarantee a decimal point.
- **Feed pre-select:** poller's `apply_fix` matches the feed's type string
  against perfdb (exact type, then model, case-insensitive) and persists
  `perf_type` (NVS, unlike RAM-only identity). Viasat `aircraftType` lands in
  `char[8]` — "A330-900" would truncate to 7 chars and match nothing; type
  codes ("A339") are what work. Display shows the resolved DB code (yellow,
  line-1 center), never the feed string; route line prefers IATA (`NOU➤NRT`).
- **`sdkconfig` drift:** the default build dir had silently become a classic
  esp32 config (no Montserrat fonts, wrong target) — regenerated the S3 config
  from `sdkconfig.defaults*` (which carry everything: fonts, TLS-insecure,
  NCM). Check `CONFIG_IDF_TARGET` in `sdkconfig` before flashing.
- **Soft-reboot after a /dfu-flash cycle wedges USB — DETERMINISTIC** (2/2
  this session): flash via /dfu + autoflash boots the app cleanly (portal
  answers over the cable), but the FIRST subsequent soft reset (`/save`'s
  `esp_restart`) leaves macOS with a stale NCM interface, no downloader port,
  AP still beaconing — the app runs (NVS write landed before the reboot),
  only USB enumeration is dead. Recovery: replug or one RST tap. Ritual:
  after every /dfu-flash, **tap RST once before anything that soft-reboots**.
- **Display-task stack panic (the 1 s-then-reboot loop):** the profile
  engine's ~1.3 KB breakpoint table lived on the display task's 3 KB stack —
  first valid fix → `etap_update` → overflow → panic, every boot. Big scratch
  buffers in pure modules belong inside the caller-owned state struct (BSS);
  display task stack now 4 KB. Symptom fingerprint: web `/status` looks fine
  in short windows, hw card shows `uptime 0:00:06 · last reset: panic`.
- **Safari ignores `hidden`/`disabled` on `<option>`** — the portal's
  make→type dependent dropdown must REBUILD the option list in JS
  (innerHTML), not hide options.

## 2026-07-09 — ETA v3: immediate appearance, blends into steadiness

- The 240 s blank warm-up bothered in practice. `eta_update()` now takes the
  instantaneous derived GS again and **blends it with the window made-good
  speed by coverage w = span/600 s**: ETA shows from the first fix (livelier
  early), and both the EMA time constant (12→120 s) and the display
  hysteresis (18→90 s) ramp up with w — a seamless transition into the
  fully steady long-window behavior. Host test asserts: appears on the first
  sample, never blanks mid-flight, ≤2 displayed-minute changes after the
  window is full despite ±10 kt oscillation.
- **Follow-up fix:** on the x10 bench, derive.c's teleport rejection reports
  gs=0 (each fix step implies ~4800 kt > its clamp) — and blending that bogus
  0 sank the early estimate below the 80 kt floor, hiding the ETA the blend
  was supposed to surface. Now the instantaneous speed only participates
  when itself ≥ 80 kt; otherwise the window made-good speed carries the
  estimate alone (appears ~10-15 s after boot). Regression case in the test
  (gs_inst=0 throughout).

## 2026-07-09 — Feed blip = location RECEIVED; devkit LED scheme v2

- **`pos_fix_seq()`** (pos.c): counter bumped on every valid fix written
  (real or emulator). The display feed icon and the LED magenta blip now
  watch it — the old `adbp_push_seq()` trigger only fired on ADBP *sends*,
  so with no EFB subscribed the feed icon stayed gray forever (user report).
- **Devkit WS2812 scheme v2** (statusled.c): slow red = no Wi-Fi, fast
  orange = scanning, steady orange = connected/no internet, steady yellow =
  connected+internet weak signal (−70 ±3 hysteresis), steady green =
  connected+internet strong, magenta 250 ms blip = location received (any
  steady state). The blue data-sent blip is gone — superseded by magenta
  fix-received per user spec.

## 2026-07-09 — Screen freeze: LVGL pool exhaustion hangs, doesn't crash

- **Symptom:** display frozen, everything else alive (web, poller, ADBP).
  **Cause:** `LV_USE_ASSERT_MALLOC=y` + LVGL's default assert handler is
  `while(1);` — a failed alloc from LVGL's builtin pool silently hangs the
  render task *while holding the render lock*. No panic, no reboot, no log.
  The 16 KB pool (set during the black-screen fix when the UI was 6 labels)
  was ~84% full with today's UI; the trip bar's horizontal gradient allocates
  its LUT from that pool per draw → eventual alloc failure → freeze.
- **Fix:** pool 16→24 KB (boot usage now 55%, stable under active gradient
  rendering; internal heap min 31 KB — still fine for TLS). **Permanent
  diagnostics** in display_task: 60 s heartbeat with `lv_mem_monitor()`
  stats into /log, plus a "LVGL lock stuck 2+ s" warning that distinguishes
  a hung render task from a crashed refresh.
- Rule of thumb: any new LVGL feature that draws per-frame allocations
  (gradients, masks) needs a pool-usage check via the /log heartbeat.

## 2026-07-09 — Trip-completion bar on the display

- Second line (below tail/type/flight): `lv_bar` 254×5 px rounded track
  (portal --line 0x1E2A44), indicator with a horizontal cyan→green gradient
  (`bg_grad_color` + `LV_GRAD_DIR_HOR` on `LV_PART_INDICATOR`), the generated
  ➤ route arrowhead riding the fill tip (label repositioned every refresh:
  tip ≈ 20 px into the glyph box), percentage 16pt right-aligned.
  Completion = 1 − remaining/(dep→arr great-circle), clamped 0..100 — early
  off-track legs can make remaining > total, hence the clamp. Hidden until
  origin+destination resolve and the fix is valid. Local clock 32→24pt
  (montserrat_28 tried in between; 28 stays enabled in sdkconfig).

## 2026-07-08 — Steady ETA on the distance line (eta.c, host-tested)

- **Naive now+dist/GS is undisplayable**: on a 4000 NM leg a ±10 kt GS wiggle
  swings it ±10 min, and slow oscillations pass through any reasonable EMA
  (the first EMA-only implementation failed its own host test). Design that
  works (`eta.c`, pure): ground speed = **made-good over a 600 s window**
  (Δdist/Δt from a 5 s-decimated ring — oscillations integrate out exactly),
  then a light 120 s EMA on the arrival epoch, then the displayed minute only
  moves past a ±90 s hysteresis band. Estimate appears only after 240 s of
  history (deliberate blank warm-up), resyncs instantly past a 30 min gap,
  resets on a distance teleport (>700 kt-equivalent step = new destination).
- **Subtle bug the host test caught**: clearing the smoothed state on an NCD
  blip made recovery reseed from the instantaneous raw value — at long range
  a seconds-long position stall bends made-good speed enough to jump whole
  minutes. Blips must blank the display but keep ALL estimator state.
- UI: distance line is now spans — `4300` amber + `NM` grayed 14pt; ETA
  `12:50` amber + `z` grayed centered on the same line (BOTTOM_MID, -34).
- Bench note: ETA needs ≥80 kt made-good for 4+ min — a moving feed
  (flightsrv.py-style), not the fixed emulator, to see it.
- **x10-speed bench feeds tripped the teleport guard**: the dest-change
  detector's slack was +5 NM, but a x10 replay closes ~7 NM between 5 s
  samples → ring reset every sample, ETA never appeared. Slack raised to
  25 NM (`ETA_JUMP_SLACK_NM`) — real destination changes jump hundreds of NM
  and still trip; regression case added to the host test.
- **Adding a new Kconfig symbol to a live `sdkconfig` by inserting a line
  does NOT work** if the file already contains `# CONFIG_X is not set`
  further down — regeneration keeps the not-set entry and drops the insert.
  Replace the `is not set` line itself (plus sdkconfig.defaults.<target> for
  fresh configs). Cost one failed build chasing `lv_font_montserrat_28
  undeclared`. Local clock is now 28pt (was 32).

## 2026-07-08 — Identity is now RAM-only (supersedes the NVS persistence)

- **Reboot forgets identity, by design** (user decision): ac_tail/ac_type are
  never loaded from or saved to NVS anymore — empty on every boot, the display
  shows its splash row, and the live feed refills identity within one poll.
  Older firmware's stored ac_tail/ac_type NVS keys are silently ignored (no
  migration needed). Supersedes the "persists so a reboot mid-flight keeps
  it" behavior from earlier today, and makes the F-XXXX migration moot.
- **Hardened /dfu confirmed working on its first real cycle**: with
  `usb_ncm_stop()` in the running firmware, the ROM downloader enumerated
  cleanly (cu.usbmodem101) right after the /dfu click — no BOOT+RST needed.

## 2026-07-08 — No-identity splash row + auto-incrementing build number

- **Display splash row** (display.c): until a tail is known (live or NVS
  last-known), the top row shows the portal-hero mirror instead of
  tail/type/flight — "AIDlink" logo (portal colors #22d3ee/#34d399), build
  number center (muted #8aa0c0), AID IP right in a badge-style framed pill
  (LVGL label with border+radius+padding). refresh() swaps visibility with
  LV_OBJ_FLAG_HIDDEN. Legacy NVS placeholder tail "F-XXXX" is migrated to
  empty on config load (it was a config default, not fed identity — it would
  have defeated the splash forever).
- **Build number** (`FW_BUILDNUM`): `tools/bump_buildnum.py` bumps
  `firmware-idf/buildnum.txt` (committed, shared by both targets) and emits
  `buildnum.h` into the build dir on EVERY build via an always-run CMake
  custom target (`add_dependencies(${COMPONENT_LIB} buildnum)`). Shown on
  the splash row, in the portal hero center (badge margin-left:auto zeroed
  inline so margin:0 auto centers it), and inside fw_build() ("… b12 …").
  Note: each target's build bumps the shared counter, so numbers are unique
  per build, not consecutive per device.
- The hardened /dfu (clean TinyUSB detach) wasn't in the *running* firmware
  for this cycle, yet the downloader enumerated anyway — wedge is a coin
  flip; the fix gets its first real test on the next update cycle.

## 2026-07-08 — Identity from the feed only + /dfu clean-detach fix

- **Aircraft identity card removed from the portal.** `ac_tail`/`ac_type` are
  now feed-tracked last-known values (persisted to NVS once per change in
  poller.c apply_fix), never user-set. Defaults are empty → ADBP answers NCD
  and the display stays blank until the feed provides identity. Existing
  units keep whatever NVS already holds as the starting last-known.
- **AID Web API version is code, not config:** `AID_API_VERSION "3.1"` in
  config.h (documented: protocol surface, EFB detection depends on it, never
  per-aircraft). `api_ver` removed from cfg/NVS/portal.
- **Type-from-feed plumbing:** parsers gained an `actype` out-param — Viasat
  `aircraftType`, Panasonic `td_id_airframe_model` — but NEITHER pinned live
  capture carries a type field; the keys are best-effort guesses to confirm
  against a future capture. Host test updated (asserts actype empty on the
  real capture).
- **/dfu wedge root-caused-ish + fixed:** 3 of 5 soft-DFU attempts left macOS
  holding the stale TinyUSB NCM device (0x303A:0x4000) with no downloader
  port — recovery was physical BOOT+RST. dfu_task now calls `usb_ncm_stop()`
  (`tinyusb_driver_uninstall()`) + 400 ms before forcing download boot, so
  the host sees a real detach. Verify over the next few /dfu cycles.

## 2026-07-08 — Status icon row v2: signal bars, internet globe, feed flash

- **Wi-Fi fan is now 3 discrete signal bars**: three concentric `lv_arc`
  quarter-fans (225..315°, diameters 8/14/20) + a 4 px dot, individually
  colorable — a font glyph can't do per-bar coloring. Weak = 1 orange bar,
  medium = 2 yellow, strong = 3 green (unlit bars dimmed 0x3A3A3A); bands at
  −70/−60 dBm with ±3 dB hysteresis each. Scanning/no-connection keep the
  fast-orange/slow-red all-bar blinks. lv_arc needs knob removed
  (`lv_obj_remove_style(a, NULL, LV_PART_KNOB)`) + indicator arc opa transp
  or it draws a slider.
- **Internet ≠ uplink: frugal active probe** (`netcore.c inet_task`): a bare
  TCP handshake to 1.1.1.1:53 / 8.8.8.8:53 alternating (SYN/SYN-ACK/RST,
  ~200 bytes, no payload, no DNS query — onboard data is metered). 30 s
  cadence while reachable, 15 s while not, prompt probe on STA-got-IP,
  false immediately on STA disconnect. **Accepted false-green:** captive
  portals usually pass/intercept port 53 pre-auth, so the globe can show
  green behind an unauthenticated hotspot; the content-validated fix (HTTP
  generate_204 → 204=internet / other=captive / timeout=none, ~700 B) was
  proposed 2026-07-08 and declined — cheap handshake is the deliberate choice.
  Globe icon = generated single-glyph font (`tools/gen_globe_font.py` →
  `font_globe.c`, U+1F310, ring+equator+meridian drawn programmatically like
  the arrow); built-in Montserrat has no globe.
- **Feed activity icon**: `LV_SYMBOL_UPLOAD`, magenta for 180 ms on each
  `adbp_push_seq()` change (same trigger as the LED blue-flash), dimmed idle.
- This round's `/dfu` soft-entry re-enumerated fine — the earlier wedge is
  intermittent, not deterministic. Keep autoflash running before triggering.

## 2026-07-08 — Display Wi-Fi indicator + T-Display /dfu re-enumeration gotcha

- **Wi-Fi indicator on the flight display** (display.c `wifi_indicator()`):
  `LV_SYMBOL_WIFI` label (16pt, built-in Montserrat fonts carry the FA symbols)
  below the tail, top-left second line. Slow red blink ≈0.8 Hz = no uplink,
  fast orange ≈3 Hz = scanning, steady green = connected RSSI ≥ −70 dBm,
  steady orange = connected but weak — ±3 dB hysteresis (−67/−73) so a
  hovering RSSI can't flicker the color. New `netcore_sta_rssi()`. The display
  task now ticks at 100 ms (blink cadence) and runs the heavier content
  `refresh()` every 5th tick — same 500 ms cadence as before.
- **Which board is on the cable? Check ARP, not assumptions:** the NCM
  host-side entry for 172.20.1.1 shows base-MAC+1 (`d0:cf:13:32:2f:49` =
  T-Display, `e8:3d:c1:f7:a2:5b`-side = devkit). Mid-session the devkit was
  swapped for the T-Display and the Mac interface silently moved en18→en12.
- **T-Display /dfu soft-entry can wedge:** after `/dfu` (FORCE_DOWNLOAD_BOOT +
  esp_restart) the ROM downloader sometimes never re-enumerates — macOS keeps
  the stale TinyUSB NCM identity (0x303A:0x4000 "Espressif Device"), no
  cu.usbmodem port, network dead. Recovery: hold BOOT + tap RST (or replug
  holding BOOT); `tools/autoflash-idf.sh` then catches the port. This flash
  booted straight into the app afterwards — the download-mode re-latch did
  not occur this time.

## 2026-07-08 — Hardware card in portal + password-less reflash path

- **New 🔩 Hardware card** at the top of the config page (web.c
  `send_hw_card()`): board profile (name + peripherals from `board_get()`),
  chip model/rev, cores + CPU MHz, radio features, physical flash size
  (`esp_flash_get_physical_size` — reports the real 16 MB even with an 8 MB
  config), PSRAM (heap-visible; "not enabled" while SPIRAM stays off), eFuse
  base MAC, ESP-IDF version, internal heap free/min, uptime + last reset
  reason. Rendered as a plain `.ctbl` table (`.hwt` label styling), NOT form
  fields — boxed inputs suggest editable data (user feedback). Use
  `ESP_IDF_VERSION_*` macros, not `esp_get_idf_version()`: the runtime string
  is a bare commit hash when the IDF checkout isn't on a release tag.
- Portal sessions are single-token: any new login mints a new token and
  silently logs out every other session (curl scripts vs browser).
- **Devkit reflash needs no portal auth:** the S3 devkit's CH343 UART port
  has the DTR/RTS auto-download circuit, so `tools/autoflash-idf.sh` +
  esptool `--before default_reset` enters the bootloader by itself — /dfu
  (and the portal password) is only required on the single-port T-Display-S3.
  This unit's portal password is NOT the default admin/password.
- **Dual-target check without clobbering the s3 build dir:**
  `idf.py -B build-esp32 -DIDF_TARGET=esp32 -DSDKCONFIG=$PWD/build-esp32/sdkconfig build`.
- The Mac's Wi-Fi being on 172.20.4.x/20 overlaps the AID subnet; the /26 on
  the NCM interface should win the route, but curl to 172.20.1.1 only behaved
  with an explicit `--interface en18` — use it in scripts.

## 2026-07-08 — In-flight round 2: GS/track derivation, EFB data quality

- **Derived GS flapped 0↔1500 kt, track swung wildly** (visible in the portal;
  suspected cause of Jeppesen FD "no AID found" after discovery — all four AID
  Web API probes answered correctly, so detection-level was fine). Root cause:
  the live feed repeats its position between ~10 s avionics updates while we
  poll at 1 Hz — naive per-poll differencing yields 0 between updates and a
  clamped spike when the jump lands in a 1 s window. Fix: `derive.c` (pure,
  host-tested): baseline advances only on real movement (>0.03 nm), EMA on
  speed, **vector EMA on heading** (wrap-safe through 360°), teleport-spike
  rejection, speed→0 only after 30 s of proven stationarity. **In-flight
  verified:** GS 507–513 kt steady (TAS 474 + wind), track 127.1–127.8true
  (mag hdg 112 + variation + drift) — vs the old 0/1500 chaos.
- Brief honest NCD blips remain when the cabin Wi-Fi drops fetches >stale_ms;
  default raised 15→30 s (NVS keeps old value on existing units — set
  `Stale timeout` 30000 in the portal).
- **`➤` for real:** npm unreachable in the walled garden, so
  `tools/gen_arrow_font.py` renders a supersampled U+27A4 arrowhead directly
  into an LVGL font C file (single glyph, padding baked into adv_w/ofs_x —
  no space glyphs needed in the span). Guarded `#if CONFIG_IDF_TARGET_ESP32S3`
  (lvgl.h doesn't exist on classic esp32 — caught by the dual-target build).
- mbedTLS printed its insecure-mode warning EVERY poll → squelched to ERROR
  level for the `esp-tls-mbedtls` tag only (deliberate config, not news).
- `tools/autoflash-idf.sh` (from the session helper): waits for the downloader
  port and flashes with retries — pairs with the portal's Firmware update
  button; transient boot-window ports make the first attempt fail ~half the
  time, hence the retry loop.

## 2026-07-07 — LIVE on the aircraft: TLS Kconfig gate + first real-feed validation

Both units ran against the real Aircalin/Viasat cabin system (ACI501). The one
bug the bench could never catch, plus the milestone:

- **HTTPS never worked until today.** poller.c was written for v9-parity
  insecure TLS (`crt_bundle_attach=NULL`), but ESP-IDF also requires
  `CONFIG_ESP_TLS_INSECURE=y` + `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y` —
  without them esp-tls refuses the handshake ("no verification option") and
  the poller reports `fetch failed` forever. Invisible on the bench (no live
  uplink); found because the Mac was on the same cabin Wi-Fi and could probe
  `wifi.inflight.viasat.com` directly (real Sectigo cert, strict verify even
  passes today — insecure mode chosen deliberately for walled-garden parity
  with v9). Now in `sdkconfig.defaults`.
- **Live validation (devkit UART log):** STA on the real client subnet
  (172.19.128.x), TLS fetches at poll rate, and
  `poll: clock set from HTTP Date (was off -1783376290 s)` — the offline clock
  chain disciplined the clock from epoch-zero on the first fetch, +7 s trim on
  the second. Bridge/NCM/web/ADBP all up simultaneously in flight.
- The captive portal tracks *client MACs* (`status:"new"`); if the AID's own
  polls are ever gated, authenticate once from any browser behind the AID's
  NAT — the portal then sees the AID's MAC.
- Live endpoint variant note: the real `/ac/flight/info` sometimes serves the
  reduced no-`attr` shape and can omit `current_time` — parser handles both
  (pinned host tests); clock survives via the Date header.
- Units renamed per-device (dev_name `aidlink2` on the devkit) — two AIDs on
  one aircraft need distinct SSIDs/mDNS names.

## 2026-07-06 — Display v2: offline clock chain, real timezones, live identity

Second iteration after live testing with replayed Viasat/FOMAX captures:

- **Clock without internet (the AID's normal life):** 3-tier UTC chain —
  (1) the position feed's **HTTP `Date` response header** parsed in poller.c →
  `settimeofday` (±3 s gate). Works in the walled garden: the time comes from
  the same onboard server as the position. (2) the fix's own `current_time`,
  ticked forward. (3) SNTP `pool.ntp.org`, opportunistic only — silent when
  unreachable, most precise when reachable. Insight from a real ground capture:
  Viasat's `current_time` **freezes when avionics updates stop** (it's
  last-update time, not now) — that's why the Date header outranks it.
- **Real timezone at position:** `tzdb.c` + generated `tzdb_data.c` — 1° world
  grid (65 KB) of zone indices from `timezonefinder` + per-zone UTC-offset
  transition tables (2026–2028). 389 IANA zones dedupe to **61** unique
  signatures. Handles DST both hemispheres, +5:30, Chatham +12:45, ocean
  nautical zones. Host-tested. **Coverage window: regenerate `tzdb_data.c`
  (`tools/gen_tzdb.py`) when it lapses (2028).**
- **Live aircraft identity:** a received tail replaces + persists `ac_tail`
  (once per change); dep/arr normalized to **ICAO** via the gazetteer wherever
  shown; `/status` gained `tail/flight/dep/arr`. Verified over ADBP: ACID
  followed the replayed tail (F-ONEA → F-ONET) with current timestamps.
- **Real Viasat capture pinned as host test:** full `attr/updated_at/value`
  wrapping, `+0000` suffix, **no groundSpeed field** (→ gs=-1 → derived from
  successive fixes — replay position jumps show as the 1500 kt clamp, by
  design). Route came as ICAO (`NWWW NFFN`), not IATA as assumed.
- **UI:** route 40pt → **32pt** (two ICAO codes overflowed 320 px), bottom row
  = `UTC+11` zone label + `12:30:12z` + local-time clock. Portal got a
  **⬆ Firmware update…** button (confirm dialog → `/dfu`) instead of a bare URL.
- **v3 avionics styling gotchas:** LVGL9 dropped label recoloring — mixed-color
  lines need **spangroups** (`lv_spangroup_*`), and `lv_span_t` is opaque in
  this LVGL: style via `lv_span_get_style(span)`, not `&span->style`. The
  built-in Montserrat fonts have no `➤` (U+27A4) — nearest glyph is
  `LV_SYMBOL_PLAY`; a literal ➤ needs a custom lv_font_conv build. `°` (U+00B0)
  IS included. New Kconfig font sizes (e.g. MONTSERRAT_24) must go in both
  `sdkconfig.defaults.esp32s3` and the live `sdkconfig`.

## 2026-07-06 — T-Display-S3 black screen: LVGL RAM starvation (+ debug toolkit)

The display stayed black although every esp_lcd call succeeded. Root cause and
the console-less debugging kit that found it:

- **Root cause: internal-SRAM starvation.** LVGL's builtin pool defaults to
  `CONFIG_LV_MEM_SIZE_KILOBYTES=64` — a **static 64 KB BSS array** in internal
  RAM, gone before boot even starts. With WiFi + TinyUSB + bridge, display init
  saw only ~52 KB free; after `lvgl_port_init`, 17 KB — and the two 320×40
  double buffers needed 2×25.6 KB DMA. `lvgl_port_add_disp` returned **NULL**
  (silent!) and no frame was ever rendered. Fix: `LV_MEM_SIZE_KILOBYTES=16`
  (frees 48 KB statically; heap at display init jumped 52→101 KB) + a single
  320×20 partial buffer (12.8 KB). A 1 Hz text UI doesn't need more. Always
  check `lvgl_port_add_disp` for NULL and log heap next to it.
- **Debugging without a console:** the board's one USB port is NCM, UART0 is
  bare header pins. Technique: `DLOG()` in display.c mirrors every bring-up
  step's `esp_err_t` into the `log.c` ring buffer → read remotely via web
  `/log` over the cable. Kept permanently.
- **`/dfu` endpoint (web.c):** writes `RTC_CNTL_FORCE_DOWNLOAD_BOOT` and
  restarts → ROM downloader on native USB, no BOOT button. Auth-gated. This
  makes single-USB-port boards reflashable fully remotely.
- **S3 download-mode latch:** after flashing over USB-Serial-JTAG (strap or
  /dfu entry), `--after hard_reset` often lands back in the downloader
  (`rst:0x15, boot:0x23 DOWNLOAD` = *forced* download, not the GPIO0 strap).
  Every soft/USB reset re-latches. **Exit requires one physical RST tap** (or
  full power-off — beware: with a battery on the JST, replugging USB is *not*
  a power cycle). Post-flash ritual: flash → tap RST.
- **ST7789 GRAM ghosts:** panel RAM survives resets — with the backlight lit
  before the first flush, the *previous* firmware's screen shows (the LilyGO
  factory demo appeared mid-debug; its Wi-Fi screen is also where the device's
  "lilygo-aabb" nickname originally came from). Backlight now stays dark until
  LVGL's first frame. Corollary: "old image on screen" ≠ "code is running".
- **Reference check (LilyGo-Display-IDF, esp-claw):** pin map, dc_levels,
  post-init order, and `disp_on_off(true)` all match ours; they run PCLK
  10 MHz (we use 8), and send a 15-cmd ST7789V vendor list (PORCTRL/VCOMS/
  gamma). Not needed for a working picture — revisit only if colors look off.

## 2026-07-06 — LilyGO T-Display-S3 (Board 3) + onboard flight display

New unit probed (`d0:cf:13:32:2f:48`, S3 v0.2, 16 MB quad flash, 8 MB PSRAM,
single USB-C = native USB, no UART bridge → LilyGO T-Display-S3). Added a
flight display: tail, flight no, DEP→ARR, NM-to-arrival, UTC offset + local
time at position (`display.c` + `airports.c` gazetteer + `board.c` identity).

- **One binary, per-board hardware:** boards are identified by eFuse MAC in
  `board.c` (devkit = WS2812, T-Display = LCD). Runtime detection of an i80
  panel isn't practical (the bus is write-only as wired) — a fleet MAC table
  is simpler and safe. New units: probe MAC, add a row.
- **GPIO48 collision:** on the T-Display-S3, GPIO48 is **LCD data D7**; on the
  devkit it's the WS2812. Driving the LED strip there would corrupt the display
  bus mid-write, so `statusled_start()` now gates on `board_get()->has_ws2812`.
- **Native-USB flashing works on this board** (unlike the devkit's boot-loop,
  which is confirmed board-specific, not an S3-generic issue). One port only:
  after AIDlink boots, TinyUSB-NCM replaces USB-Serial-JTAG — the vanishing
  serial port is the *success* signal. Reflash by holding BOOT while plugging.
  No console at all in practice; verify via NCM lease + `/status`.
- **Display stack:** IDF-native `esp_lcd` i80 ST7789 (320×170 landscape,
  y-gap 35, invert on, swap_xy + mirror-y, 8 MHz PCLK) + LVGL 9 via
  `espressif/esp_lvgl_port`, both rule-gated `target == esp32s3` in
  `idf_component.yml`. Classic esp32 map: 0 lvgl/esp_lcd symbols. Backlight
  (GPIO38) held dark until the first frame is rendered — no boot noise flash.
- **Timezone at position:** solar estimate `round(lon/15)` (a real IANA tz
  lookup needs a shapefile DB). Clock ticks from the fix's UTC timestamp
  (`utc_ms + elapsed`), so it's correct with SNTP never running.
- **Bench verify without a console:** served a fake Viasat feed from the Mac
  (`scratchpad/flightsrv.py`, NOU→NRT @ 470 kt, live UTC), set src=custom via
  a scripted `/save` POST (mind the presence-toggles: include `staDhcp`,
  `napt`, `authEnable` or they silently turn off). Device polled 1 Hz over the
  cable, `/status` tracked the moving fix. Full chain poller→pos→display live.

## 2026-07-05 — L2-bridge the Wi-Fi AP + USB-NCM so the AID is at 172.20.1.1 on both

- **Problem:** EFB hard-codes the AID at `172.20.1.1`. That was only the Wi-Fi AP
  IP; over the USB cable the ESP32 was `172.20.2.1/29`, so iOS (which won't route
  off-subnet traffic out a USB-Ethernet adapter) never reached the AID.
- **Fix:** lwIP bridge (`CONFIG_ESP_NETIF_BRIDGE_EN`). A `BR_DHCPS` bridge netif
  owns `172.20.1.1/26` + DHCP pool + NAPT-to-STA; the Wi-Fi AP and USB-NCM are
  L2-only ports (`flags=0`/`AUTOUP`, `ip_info=NULL`, no per-port DHCP). Both
  Wi-Fi and cable clients now land on `172.20.1.0/26` and hit the AID directly.
- **Gotchas (all cost a boot loop / dead bridge):**
  - The `esp_netif_br_glue` port machinery is `ETH_EVENT`-driven — a custom
    TinyUSB netif added via `esp_netif_br_glue_add_port` is silently never bridged.
    Add it with the public `esp_netif_bridge_add_port(br, usb)` directly, *after*
    both bridge and port are started (both need a registered `lwip_netif`).
  - Don't call `esp_netif_action_start(br)` yourself — the glue does it on
    `WIFI_EVENT_AP_START` (its own `br_started` flag); a manual start double-adds
    → `assert netif_add (netif already added)` boot loop.
  - The glue only brings the bridge **link up** on `WIFI_EVENT_AP_STACONNECTED`
    (first Wi-Fi client). For USB-only use, call `esp_netif_action_connected(br)`
    yourself after AP_START, then start DHCP — else DHCP never runs & NAPT fails.
  - Override the `BR_DHCPS` default `ip_info` (192.168.4.1) before `esp_netif_new`;
    keep `bridgeif_config_t` static (retained by the netif).
- **iOS routing insight:** service order only picks the *default route*; on-link
  subnets always use their own interface. Making the AID on-link on USB is why
  this works regardless of the iPad's Wi-Fi priority. Caveat: if the iPad is also
  on FOMAX Wi-Fi at `172.20.1.x`, that's an unresolvable on-link clash.

## 2026-07-05 — AP-client DNS black-hole (intermittent DNS "not forwarded")

- **Symptom:** Devices on the AIDlink AP (esp. iPhone/iPad reaching the Viasat
  endpoint) intermittently failed DNS resolution.
- **Root cause (confirmed on hardware):** `startAP()` handed clients a DNS server
  via DHCP that was wrong/dead depending on uplink timing. When the STA uplink was
  not yet associated at AP-start, `WiFi.dnsIP()==0` and the code fell back to
  `dns=ip` → advertised **172.20.1.1** as the resolver, but the firmware ran **no
  DNS server** there. Clients cache the DHCP-provided resolver for the whole lease
  (**default 120 min**), so DNS stayed dead even after the uplink recovered. iOS is
  especially aggressive about caching the leased resolver.
- **Fix:** Added a small on-device UDP **DNS forwarder** (listens on AP `:53`,
  relays each query to the *live* `WiFi.dnsIP()` / `apClientDns`, NAT-style txn-id
  remap in `dnsPend[]`). `startAP()` now always offers **172.20.1.1** as the client
  DNS. Because the upstream is resolved per-query (not baked into the lease), DNS
  follows STA reconnects and uplink-DNS changes and never goes stale.
- **Also:** re-assert the DHCP DNS-offer (`OFFER_DNS` /
  `ESP_NETIF_DOMAIN_NAME_SERVER`) inside `applyDhcpPool()` after its
  `dhcps_stop()/start()`, so the offer can't be dropped by that restart.

### ESP32 Arduino core 3.3.10 gotchas (WiFi/NetworkInterface)
- `WiFi.softAPConfig(ip,gw,mask,lease,dns)` → `NetworkInterface::config(local,gw,
  subnet,dns1,dns2,dns3)` with **dns1=lease-start, dns2=dns**. The DHCP DNS handed
  to clients comes from **dns2**, and `OFFER_DNS` is only enabled when `dns2 != 0`.
- The device's *own* resolver (Viasat poll, NTP) uses the **STA** netif DNS, not the
  AP netif — setting AP-netif DNS MAIN to 172.20.1.1 for the DHCP offer does not
  affect the device's outbound lookups.
- Bench note: `tools/flash.sh` piped to `tail` can report a non-zero exit while the
  upload actually succeeds; verify with the boot banner version over serial.

### Verification status
- Structural fix confirmed on hardware (forwarder up on :53; clients offered
  172.20.1.1; correct `(uplink down)` reporting). **Live positive-path resolution
  still to be confirmed with the real Viasat/Aircalin uplink present** — with the
  uplink up, a client can test: `nslookup wifi.inflight.viasat.com 172.20.1.1`.

## 2026-07-05 — ESP-IDF rewrite (Milestone 1) hardware gotchas

Rewriting AIDlink on ESP-IDF v5.3.5 (one codebase, targets `esp32` + `esp32s3`;
new project in `firmware-idf/`). Key things that cost time:

- **ESP-IDF install didn't include cmake/ninja** (only compilers). Fix:
  `python3 $IDF_PATH/tools/idf_tools.py install cmake ninja`.
- **NAPT Kconfig was renamed.** `CONFIG_LWIP_IP_NAPT` (old) is silently ignored on
  v5.3 → `esp_netif_napt_enable()` returns error and logs `NAPT FAILED`. Correct
  symbols: `CONFIG_LWIP_IP_FORWARD=y` **and** `CONFIG_LWIP_IPV4_NAPT=y`.
- **ESP32-S3 native-USB flashing boot-loops this board.** Flashing via the native
  USB (USB-Serial-JTAG, VID 0x303A, `/dev/cu.usbmodem101`) leaves the 2nd-stage
  bootloader crashing on entry (`rst:0x7 TG0WDT_SYS_RST`, never banners). **Stock
  `hello_world` fails identically** → it's the board/native-USB path, not our code.
  **Flash + monitor the S3 via its CH343 UART port** (VID 0x1A86,
  `/dev/cu.usbmodem5AE6043xxxx`) instead — boots fine there. The native USB is then
  free for TinyUSB NCM (M1/T5). Console = UART on the S3 for this reason.
- **Octal PSRAM deferred.** `CONFIG_SPIRAM_MODE_OCT` isn't needed for M1; left off
  (it was an early red herring while chasing the boot loop). Revisit in M5.
- Host-testable pure logic isolated in `*_util.c` (no IDF deps), tested with a
  plain `clang` assert runner — avoids the IDF linux-target/Unity machinery.

### Milestone 1 — COMPLETE (acceptance results)

All 7 tasks done on branch `esp-idf-rewrite`. Evidence:

- **Host unit tests:** `config_subnet` 5/5 PASS, `dnsfwd_remap` 6/6 PASS (plain clang).
- **Dual-target builds:** both `esp32` and `esp32s3` build clean. Classic `esp32`
  map has **0 tinyusb symbols** (USB-OTG guard verified).
- **S3 hardware (flashed via CH343 UART):** boots; SoftAP `AIDlink` up
  `172.20.1.1/26` NAPT ON; USB-NCM up `172.20.2.1/29` DHCP+NAPT; DNS forwarder
  on :53; TinyUSB driver installed — all simultaneously, no crash.
- **USB-C cable = network interface (the feature):** Mac `en12` leased
  **172.20.2.2** over the cable; ping gateway 172.20.2.1 **0% loss ~1.3 ms**;
  Mac installs **default route via the cable**. Coexists with the AP (both
  subnets served at once).
- **Not yet verifiable on the bench:** full DNS/internet-through-NAT (needs a live
  WiFi uplink + a configured STA SSID — the web UI to set it is M2; the forwarder
  correctly drops queries with no upstream). Autodetect-on-unplug (needs physical
  replug) — deferred to a live session.

### Extra gotchas found during M1

- **`idf.py set-target` + stale `build/`:** switching target without clearing
  `build/` leaves `flasher_args.json` on the old `--chip`, so `flash` fails with a
  cryptic esptool error. `flash-idf.sh` now force-cleans on target change (exact
  match, so `esp32` ≠ `esp32s3`).
- **USB-NCM RX must copy:** TinyUSB owns the rx buffer only for the callback's
  duration; `esp_netif_receive` is async → copy the frame and free it in
  `driver_free_rx_buffer`.
- **Two distinct MACs:** the S3-side netif MAC and the host-side NCM MAC must
  differ or ARP won't resolve over the cable.

## 2026-07-05 — M2 web config portal (esp_http_server) done

Ported the v9 web portal + auth to ESP-IDF. Verified end-to-end **over the USB-C
cable** (curl to the NCM gateway 172.20.2.1): login (admin/password), cookie
gating (protected 302 vs public API), full config page (7 cards / 38 fields),
/save → clamp → reboot, and **settings persist in NVS** (saved staSsid survived a
reboot and showed in /status). Both targets build; classic esp32 = 0 tinyusb.

Gotchas:
- **Chunked-encoding terminator bug:** `httpd_resp_sendstr_chunk(r, "")` writes a
  zero-length chunk = the HTTP end-of-response marker. An empty field value
  (blank staSsid) truncated the page mid-render (2 KB vs 6 KB). Fix: the chunk
  helpers skip empty strings.
- **Managed-component wedge:** after adding deps to `REQUIRES`, the build failed
  in `espressif__tinyusb` CMake (`rndis_reports.c` add_library). `rm -rf
  managed_components dependencies.lock build` + reconfigure fixed it.
- `esp_app_get_description()` needs `esp_app_desc.h`; `time/gmtime_r/strftime`
  need `<time.h>`; IDF builds with `-Werror=misleading-indentation` (no two
  `if`s on one line).
- `/status` position fields are placeholders until the M4 poller lands.

## 2026-07-05 — M3 ADBP ARINC-834 server done

Ported the ADBP feed to ESP-IDF (lwIP sockets). Split pure wire-format logic into
`adbp_frame.c` (host-tested, incl. the self-referential `length=` fixed-point
iteration) and the socket/task layer in `adbp.c`. Shared ownship state in
`pos.c` (mutex-guarded).

Verified over the USB-C cable (Python TCP client to 172.20.2.1:24000):
- `getAvionicParameters` → valid `<response errorcode="0">` with the requested
  params; unknown method → errorcode 2.
- `subscribe` (publishport 51050, 1s period) → the AID connected back to the
  Mac's port and pushed 5 frames at ~1 Hz, each framed
  `<method name="publishAvionicParameters" length="214">…` with the correct
  self-referential length.

All params read `validity="2"` (NCD) because there's no position producer yet —
that's M4 (poller/emulator writes pos_set()). Both targets build; 4 host test
suites pass; classic esp32 = 0 tinyusb.

Recurring gotcha: the `espressif__tinyusb` managed component fails CMake
reconfigure (`component.cmake:486 add_library`) whenever sources are added; fix
is `rm -rf build managed_components dependencies.lock` then rebuild. Also IDF
`-Werror=format`: uint32_t IP bytes are `unsigned long` here — cast to `(unsigned)`
for `%u`.

## 2026-07-05 — M4 position poller + sources + emulator done

Ported the poller to ESP-IDF. Pure, host-tested parsers (`poller_sources.c`,
compiled against real IDF cJSON source in the host test): Viasat nested-`value`
+ ISO8601→epoch, Panasonic flat `td_id_*` with deg×1000 sign decode. `geo.c`
(host-tested) does great-circle bearing/haversine to derive track+GS when the
source omits them. `poller.c` fetches via `esp_http_client` (insecure TLS +
`disable_auto_redirect` to mirror v9's raw-socket `setInsecure()`/no-redirect),
`apply_fix` derives+clamps, `sim_step` runs the emulator, stale→NCD watchdog.

Verified end-to-end over the cable: enabled the emulator from the web portal →
`/status` shows valid position → ADBP `getAvionicParameters` returns
`validity="1"` LAT/LON/GS/TRK/ALT/ACID/GNSS_AVAIL (track 245°→-115.00 via
norm180, ACID=F-ONEO). The full chain emulator→pos_set()→ADBP works.

Parity note: v9's emulator is a **fixed** position (fixed=true, no advance) —
matched it (dropped an initial moving-emulator version). 6 host suites pass;
both targets build; classic esp32 = 0 tinyusb.

## 2026-07-05 — M5 polish/parity + ESP-IDF rewrite COMPLETE

- **mDNS** (`services.c`): advertises `<dev_name>.local` + `_http` +
  `_aidlink-adbp` (managed component `espressif/mdns`). Boots
  `[mDNS] http://aidlink.local/`. Resolves for Wi-Fi AP clients; cable hosts use
  the fixed 172.20.2.1 gateway (mDNS doesn't bind our custom USB netif).
- **Docs**: `firmware-idf/README.md` (build/flash, cable, host tests, layout);
  main README now points to both firmwares.
- **Kept the Arduino sketch** rather than deleting it — it's the proven shipping
  build, and full parity isn't signed off until the live upstream Wi-Fi→NAT→
  internet path is confirmed on real hardware (bench has had no live uplink).
- **Deferred** (documented, low-value): the /log ring-buffer web endpoint (UART
  logging works; the IDF web UI doesn't poll /log), a cfgVer migration counter
  (per-key NVS default seeding is equivalent), and S3 16 MB partition expansion
  (the 3 MB app is 66% free).

### Final state
6 host suites pass; both targets build clean (esp32 67% / esp32s3 66% app free;
classic esp32 = 0 tinyusb symbols). On the S3, hardware-verified over the USB-C
cable: DHCP lease + NAT internet path, web portal (login/save/persist), and the
ADBP feed emitting valid ARINC-834 parameters from the emulator. The one thing
still to confirm on real hardware is the live upstream-Wi-Fi → NAT → internet
path (needs a real uplink present).

## 2026-07-05 — Post-milestone hardening + features (from field testing)

After the M1–M5 rewrite, live testing on the S3 surfaced a series of fixes and
additions (all committed):

- **Exact v9 web UI:** the M2 portal was cosmetically simplified; reproduced the
  v9 CSS/layout/fields byte-for-byte, incl. the config-field data model
  (`apIp`/`apMask`/`apLease` strings, not `ap_prefix`).
- **Wi-Fi scan returned 0:** the STA auto-reconnected on every disconnect so it
  was perpetually "connecting" (driver rejects scans then). Added `netcore_scan()`
  that pauses reconnect + disconnects to a scannable state; only auto-connect when
  an SSID is configured.
- **Save → blank page:** `esp_restart()` in-handler + a fixed meta-refresh raced
  the reboot/USB re-enumeration. Now: reboot from a deferred task; the "Saved" page
  polls `/login` until back, then navigates.
- **Blank login page — "Header fields are too long":** the real one. Default
  `CONFIG_HTTPD_MAX_REQ_HDR_LEN` is 512; a browser's cookie + User-Agent + Accept
  headers exceed it → server rejects → blank. Raised to 2048 (URI 1024). Also
  added `Cache-Control: no-store` so a blank captured mid-reboot can't stick.
- **Login expiring quickly:** dropped the 30-min server idle timeout; always
  persist the token to NVS and set a cookie `Max-Age` (7d, or 30d "remember") — a
  bare session cookie was dropped by iOS Safari on app-switch to the EFB.
- **AID not detected by Jeppesen FliteDeck:** the ARINC-834 AID Web API uses
  **POST**, but endpoints were GET-only → 405 on the EFB's probe. v9 answered any
  method. Now GET+POST, drain the POST body (keep-alive framing), and stamp a
  plausible ~2025 timestamp instead of 1970 (time() before SNTP).
- **Clients list showed "(pending)":** resolve each station MAC to its lease via
  `esp_netif_dhcps_get_clients_by_mac()`. Also list the USB-C cable host, and log
  AP client join/leave (with reason code) to the traffic log.
- **Live gw/DNS display:** the Uplink card showed blank Gateway/Netmask/DNS on
  DHCP; now reads the live STA netif via `netcore_sta_ipinfo()`.
- **Defaults:** uplink DHCP on, poll 1 s, aircraft `F-XXXX`/`A320`.
- **Onboard RGB status LED (S3, `statusled.c`):** WS2812 on GPIO48 via the
  `led_strip` component. flashing orange = scanning, solid green = connected,
  flashing red = searching, brief blue flash (~250 ms) = position frame sent. The
  board's other small LEDs are hardwired UART/USB activity (not controllable) —
  confirmed with an identify test pattern (only pixel 0 responded).

Repo hygiene: the `Desktop/AIDlink` path is **iCloud-synced**, which spawns
`name 2.ext` conflict copies; a batch of them had been committed. Removed all
`* 2.*` files (they were never in the CMake SRCS list, so harmless but confusing).
