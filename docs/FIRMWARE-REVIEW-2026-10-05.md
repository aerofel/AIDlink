# AIDlink firmware review — 2026-10-05

Scope: `firmware-idf/` at commit `1cedc61` plus the uncommitted working tree:
- the poller self-heal "degraded episode" change;
- the T-Display-S3 Touch support added today.

Lens: an unattended box on an aircraft for up to 14 h, feeding ownship to an EFB.
Two outcomes matter most:
1. The box is stuck or overloaded and the EFB loses position.
2. The box sends a wrong position marked valid.

Method:
- Five independent read-only reviews: memory, concurrency/watchdogs, network, position integrity, portal/security.
- Every item marked **verified** was re-checked in the source or build output (map file, `sdkconfig.h`).
- **plausible** means the code path is confirmed but the in-flight trigger has not been reproduced.
- A live load test on Board 6 over the cable (see the end of this document).

## Status (updated 2026-10-05, same day)

**Fixed:** #1, #2, #3, #4, #6, #9, #10, #21 and #24. They are flashed to Board 6 and verified there. Nothing is committed yet.

| # | Fix | Verification |
|---|---|---|
| 1 | `log_foreach` copies one line per lock and `flog_dump` locks per sector. Both abort on a send error. | **A/B on hardware**: a `/flog` client that stops reading blocked ADBP for **~83 s** on the old build. The new build answers in **0.01 s**. A host test with FreeRTOS stubs checks that the callback never runs under the lock and that there are no repeats or reordering. |
| 2 | Non-blocking ADBP push connect (300 ms cap), retry every 5 s after 3 failures, logs on unreachable and on recovery. A dead subscriber is evicted only when a new EFB needs its slot. | **Hardware**, refused-connect path: logged once, 5 connects in 12 s, pushes resumed 3.6 s after the EFB came back. The silent-drop path (SYNs dropped with no reply) needs root on the Mac to simulate and is **not bench-tested**. |
| 3 | Only transport failures with the probe green feed the episode, and uplink-down closes it. The restart counter resets only after 2 min clear of transport failures, counted from the restart itself. | Host tests (new cases) |
| 4 | `CONFIG_LV_MEM_SIZE=32768` | Map shows `work_mem_int` 0x8000. **Internal heap 17→49 KB free, min 8→42 KB, largest block 7→24 KB.** LVGL is 47% used with the full flight UI. |
| 6 | Event-task stack set to 4096 | Present in `sdkconfig.h` of all three builds |
| 9 | `norm180` applied to the dead-reckoned longitude | Host test (fails on the old code). **Hardware**: a fake NOU→PPT feed crossed 180° at 480 kt: 179.99787 → −179.99971, 109 samples, none beyond ±180. Dead reckoning after the feed stopped stayed continuous. |
| 10 | Known altitude sent as-is (0 and negative included); a missing altitude is sent as NCD instead of FL310. | Host tests (parser NAN, ADBP 0 / −11 / NCD). ⚠ **Still to check on the ground: whether FliteDeck keeps showing ownship when altitude is NCD.** If it doesn't, restore a stand-in value for the missing case only. |
| 21 | `LWIP_DHCPS_MAX_STATION_NUM=32` | Present in `sdkconfig.h` of all three builds |
| 24 | STA disconnect sets `INET_NO_UPLINK` | Code only (needs an uplink to exercise) |

The generated `sdkconfig` files are untracked and were edited alongside `sdkconfig.defaults*`. A fresh checkout gets the same values from the defaults.

**Step 3, also fixed:** #5, #7, #8, #11, #12, #13 and #16, plus three problems that testing uncovered. All are flashed to Board 6 and verified there.

The supervisor and watchdog tests used a throwaway fault-injection build that can suspend a named task or spin a CPU. It was never committed; the real build returns 404 on `/fi`.

| # | Fix | Verification |
|---|---|---|
| 5 | `supervisor.c`: per-task heartbeats with limits of adbp/dnsfwd/display/gps 30 s and poller 120 s. A silent task triggers a restart. The reason is parked in RTC memory and logged after the reboot, because the supervisor takes no application locks. At most 3 restarts without 30 healthy minutes. The supervisor is itself watched by the task WDT. `ESP_TASK_WDT_PANIC=y` at 10 s. Core dump to a new `coredump` partition (128 KB, 0x390000), served at `/coredump`. | **Hardware.** Suspending adbp, display and dnsfwd gave a restart at 30–31 s each, with the reason logged. The 4th stall did not restart (budget spent). A busy loop on CPU 1 gave `reset=task_wdt`, and a 37 KB dump downloaded from `/coredump` decodes with `esp-coredump` to the culprit task and source line. The old partition table (no coredump partition) boots normally and reports "no core dump stored". |
| 7 | `fixgate.c` FROZEN: a position unchanged for 30 s while moving at ≥ 200 kt is dated back to its last real change, and the ADBP hold dead-reckons from that moment. | Host tests. **Hardware**, fake feed frozen 100 s at 480 kt: FROZEN logged at +30 s and ADBP went fresh→dr in the same second. DR error vs. the true track was ≤ **0.37 NM** (a parked position would be up to 13 NM off), HIL grew 4→13 NM, and dr→fresh came back on resume. |
| 8 | `fixgate.c` BAD (NaN/inf/out of range/0,0 rejected, treated as a parse failure) and HOLD (over 1200 kt, **or** off the dead-reckoned track by more than 1 NM + 25 % of distance flown). Held until the next different position confirms it. | Host tests (the real 2026-08-16 glitch, a full 360° standard-rate turn with a lagging track that is never held, antimeridian, ms wrap). **Hardware:** the 1.5 NM sidestep 5 s after an update (1195 kt, which the speed test alone let through on the first try) is now never published. |
| 11 | FOM/HDOP/HIL/WOW go NCD with the position. HIL grows with GS×age during DR. The real GNSS HDOP is passed through. WOW/DOOR keep the v9 constant while a position exists. | Host tests |
| 12 | `/dfu`: GET only shows a confirmation page with a single-use token (2 min); POST with the token acts. The page warns while an EFB is fed. `tools/flash-aid.sh` updated. | **Hardware:** 3 GETs did nothing; bad, superseded and already-tried tokens got 403 and the unit stayed up; `flash-aid.sh` flashed through the new flow twice. |
| 13 | `esc()` escapes `'` | Code (one-line change) |
| 16 | `/save` rejects a body shorter than Content-Length | **Hardware:** a 46-of-900-byte POST got `400 incomplete form, nothing saved`, no reboot, settings intact |
| — | **Found during testing:** a planned software restart with USB attached can leave the cable link dead (known since 2026-08; it happened on the first supervisor test). Self-heal, supervisor and portal-save restarts now go through `usb_ncm_restart()`, which detaches first. | **Hardware:** 3/3 restarts after a cable flash came back in ~5 s (the old path wedged 1 of 1 here, and 1 of 3 and 3 of 5 in earlier sessions). Panics can't detach, so after a panic the cable may need an RST; Wi-Fi is unaffected. |
| — | **Found during testing:** `web_start()` turned log capture off on display boards until `display_start()`. Every boot-time line was lost: self-heal "booted by restart", touch detection, and now the supervisor and core-dump notices. | **Hardware:** those lines now appear in `/log` and `/flog` |
| — | **Found during testing:** when the feed went non-live, the arbiter kept the last *replayed* poll's timestamp, so the DR hold trailed the aircraft by 30 s (4 NM at 480 kt). | Fixed in the SRC_NONE branch. **Hardware:** a constant −4 NM error became ≤ 0.37 NM. |

**Hardening round, also fixed:** #14 (partly, see below), #15, #17–#20, #22, #23, #25–#30, the P2 list and the smaller integrity items. They are flashed to Board 6; all 20 host suites pass, and the S3, T3-S3 and classic ESP32 builds are clean.

Your decisions:
- Nothing is reachable from the uplink side.
- Default credentials are kept, but an AP passphrase under 8 characters is now rejected.
- GNSS takeover is quality-gated.

| # | Fix | Verification |
|---|---|---|
| 14 | AP passphrase must be 8–63 characters (it used to fall back to an OPEN AP). `ap_hidden`, `ap_channel` and `ap_max_clients` are now applied. `csa_count=3`. Default credentials unchanged, as you chose. | `cfg_validate` host test; hardware: a short passphrase is rejected |
| 15 | Portal, ADBP and DNS only answer on the AID's own IP. httpd `open_fn` checks the socket's local address (this also refuses IPv6 and the STA IP); ADBP and DNS bind 172.20.1.1. mDNS is off on STA and registered on the S3 bridge. | Hardware: the cable still works. `aidlink.local` now resolves over the cable (it did not before). The refusal path is code-only: there is no uplink on the bench, and the device has no IPv6 link-local address. |
| 17 | `/save` parses into a copy; `cfg_validate()` checks subnet/mask/pool/broadcast, ports, DR timings, names; nothing is applied unless everything passes; each reason is shown. | 20-case host test; hardware 400 with the list, no reboot |
| 18 | Scan no longer disconnects a live uplink | Code |
| 19 | Over-long credentials, SSIDs, device name and URL are rejected before truncation | Code (raw-length table) |
| 20 | `cfg_save` result is checked (no "Saved ✓" when NVS fails; the old config is restored in RAM). A save mutex covers the 3 writers. | Code |
| 22 | STA reconnect backoff 1/2/5/10/30 s; disconnect reason and RSSI logged | Code (needs an uplink to exercise) |
| 23 | STA all-channel scan, strongest AP. **No active roaming yet.** | Code |
| 25 | 32 sockets / 32 TCP pcbs (16 was believed to be lwIP's maximum; it is not) | Build config |
| 26 | `SPIRAM_MALLOC_ALWAYSINTERNAL` 16 KB → 4 KB | **Hardware:** largest internal block 24 → 31 KB; feed, ADBP and portal tests pass |
| 27 | Heap telemetry reports what `malloc` can really get (INTERNAL\|DEFAULT) | Hardware card / poll summary |
| 28 | DNS: replies only from the queried resolver on :53 with a matching question; SERVFAIL immediately with no uplink | Host test; **hardware: SERVFAIL in 22 ms** |
| 29 | Log ring, ADBP buffers and DNS cache (~48 KB) moved to PSRAM | Static DIRAM free 80 → 121 KB |
| 30 | UDP recv mbox 16; NAPT ports 30000–45000 (off lwIP's own 49152+) | Build config |
| P2 | Touch self-recovery; overlay timer wrap; `arbitrate` order; reboot-task fallback; `/flog` clear and `/logtoggle` POST-only; feed strings escaped in HTML and JSON; per-request log lines RAM-only; 10-min `[mem]` stack/heap report; button and inetprobe stacks 4 KB; lwIP tcpip stack 4 KB (the bench showed 1276 B left) | Hardware: GET toggles 405, `[mem]` line present (every task ≥ 1.6 KB free) |
| — | **The flash log now always captures.** The "Live capture" toggle used to gate it, and unticking it silenced post-flight evidence. The toggle now only drives the `/log` view. | Code |
| — | Login brute force: 5 failures → 30 s lockout, replacing a 700 ms sleep inside the single httpd task | Hardware: 6th attempt 429, portal still answers in 50 ms |
| integ. | GNSS gate (3D, HDOP ≤ 5 held 10 s in; out on loss or HDOP > 10); GPS fix time stamped only by GGA; `""` / `"N/A"` read as absent, not 0; Panasonic heading no longer used as track; ISO time-zone suffix honoured; HTTP Date accepted only 2025–2038; display minutes no longer show `°60.00` | Host tests (gate, parsers); **the gate is not tested with real GNSS** (Board 6 has none) |

**Second review (of today's own changes).** 10 findings, of which 8 were fixed, 1 was not a bug and 1 was kept as a trade-off. All 20 host suites pass, the 3 builds are clean, and Board 6 was reflashed.

| Finding | Outcome |
|---|---|
| Supervisor `now - s_last` was unsigned: a beat landing on the other core between the two reads wrapped to ~4e9 ms and **restarted a healthy unit** | Fixed: signed compare |
| Self-heal: one timeout a minute kept the episode open forever, so a ~98 % healthy link was restarted | Fixed: `heal_episode_dense()`, at least 50 % of the episode's polls must fail (host-tested: 30 min at 1 fail/min → no restart) |
| `alt_unknown` reached ADBP only; the display showed "0" and the ETA profile used 0 ft | Fixed: display shows `---`, ETA gets −1 |
| `/save` swapped the live CFG outside the save lock (torn NVS write, or a rollback discarding a concurrent key press) | Fixed: `cfg_commit()` copies, saves and rolls back under the lock |
| ADBP connect capped at 300 ms: a power-saving iPad (DTIM ~300 ms) could fall into slow retry or be evicted | 1 s cap |
| Login lockout was one global counter: one client could lock the crew out indefinitely | Per-client table (8), failures forgotten after 10 min |
| Duplicate great-circle code (fixgate vs DR), dnsfwd re-implementing `netcore_downstream_ip()`, ADBP IP formatting repeated | `geo_dest()` in geo.c (wraps ±180), shared helpers |
| ADBP per-accept `getsockname` dead under the IP bind; stale "16 is the maximum" comment | Removed / fixed |
| "GGA-only fix stamping breaks RMC-only receivers" | **Not a bug**: this parser only ever took the position from GGA (`have_pos`) |
| All-channel STA scan lengthens each reconnect | **Kept**: joining the strongest cabin AP matters more, and the backoff now bounds the cost |

Regression on hardware: glitch never published; 180° crossing clean (109 samples). Freeze DR error is ≤ 0.16 NM after a 2-min warm-up. A run frozen only 50 s after boot showed 2.4 NM: derived GS converges over ~60 s from a cold start (272 kt at 15 s, 440 kt at 30 s, 471 kt at 45 s), so a freeze in that first minute dead-reckons slow. GS also overshot ~8 % for a few seconds after a 100-s gap.

**Deliberately not done:**
- **TLS certificate verification stays off.** This is v9 parity: the cabin portal certificates and captive interception make it a reliability risk.
- **No active STA roaming.**
- **Panic restarts can't detach USB first**, so the cable link may need an RST after a panic.
- **One global portal session**, so logging in elsewhere logs you out.
- **DNS answers over 1400 B are cut without the TC flag.**
- **The cJSON feed is still parsed twice per poll.**
- **USB-NCM receive copies still use internal RAM.** Moving them to PSRAM needs a throughput test.

Also: a FROZEN feed is only recognised after 30 s. In addition, a FROZEN feed is only recognised after 30 s. Until then the EFB sees a repeated position, which is how the portal normally behaves between its ~10 s updates.

---

## P0: can freeze the feed, reboot-loop, or waste the scarce RAM

### 1. `/log` and `/flog` hold the log locks while sending over the network (verified)
- **Where:**
  - `log.c:43-49`: `log_foreach` holds the ring mutex across `web.c:927` `log_emit`.
  - `flog.c:118-141`: `flog_dump` holds the flog mutex for the whole 512 KB stream.
  - Send errors are ignored, and httpd waits up to 5 s per send.
- **Why it freezes things:** every `logln()` takes the same lock with `portMAX_DELAY`. The callers are:
  - the ADBP task, on every request (`adbp.c:127`) and on push;
  - the display task, while it holds the LVGL lock;
  - the poller;
  - the Wi-Fi event handler.
- **Scenario:** a crew phone or iPad opens `/log` or `/flog`, then locks or leaves range mid-transfer. ADBP, the display and Wi-Fi reconnects then block for seconds to many minutes. The EFB loses position and the screen freezes.
- `display_start()` forces logging on, so every display board is exposed.
- This is a strong candidate for at least some of the past "stuck" events.
- **Fix:**
  - Copy under the lock, send after releasing it.
  - For flog, read one sector, unlock, then send.
  - Abort on the first send error.

### 2. A departed EFB blocks the single ADBP task (verified)
- **Where:**
  - `adbp.c:199`: blocking `connect()`. `SO_SNDTIMEO` does not bound an lwIP connect, so the SYN retries run their full course.
  - Subscriptions never expire. They are cleared only by an unsubscribe or a re-subscribe from the same IP (`adbp.c:140`).
- **Scenario:** one iPad sleeps, leaves the AP or is unplugged without unsubscribing. On every push period the ADBP task sits in `connect()`. The other EFB's pushes and `getParameters` requests stall.
- Four stale slots means a new EFB silently gets no slot. The no-slot case is only `ESP_LOGW`, so it never reaches `/log`.
- **Fix:**
  - Non-blocking connect plus `select` of 300–500 ms or less.
  - Drop a subscription after N consecutive failed connects.
  - Log the no-slot case with `logln`.

### 3. The uncommitted self-heal can reboot every ~10 min for a whole flight (verified)
- **Where:**
  - `poller.c:409`: `s_heal_n = 0` on any single success.
  - `poller_heal.c:30-31`: an episode closes only after 2 min with no failure.
  - `poller.c:320`: every failure feeds the episode, including `parse`/`HTTP xxx`.
- **Scenario:** a degraded satellite link (the 2026-09-04 / 09-19 pattern) with one failure every under 2 min while the probe is green.
  - It restarts at 10 min.
  - The first success after boot resets the counter.
  - It restarts again at 10 min, and so on.
  - `HEAL_MAX_RESTARTS` never applies.
- The episode clock also runs while the STA is down, so the first failure after reconnect can reboot at once.
- Each reboot drops the AP, the NCM link and the ADBP subscriptions.
- **Fix before committing:**
  - Reset `s_heal_n` only when the episode closes.
  - Feed the episode only with `heal_transport_err()` failures.
  - Count episode time only while `inet == OK`.
  - Require a failure density (e.g. at least 80 % of attempts) rather than "one failure per 2 min".

### 4. The LVGL pool is 64 KB of internal SRAM, not 24 KB (verified in the map)
- `sdkconfig:2301` has `CONFIG_LV_MEM_SIZE=65536`, which wins over the deprecated `CONFIG_LV_MEM_SIZE_KILOBYTES=24`.
- `work_mem_int` is `0x10000` in `.bss`.
- The heartbeat shows about 15 KB actually used (`lv used=23%`).
- **That is about 40 KB of the scarcest RAM on the board, twice today's whole free internal heap.** It affects every display board.
- **Fix:**
  - Set `CONFIG_LV_MEM_SIZE=24576` in `sdkconfig.defaults.esp32s3`, or move the pool to PSRAM.
  - Re-check `lv used=%` afterwards.

### 5. No watchdog for application tasks, and LVGL's out-of-memory assert still hangs (verified)
- `esp_task_wdt_add` is never called, and `CONFIG_ESP_TASK_WDT_PANIC` is off. A hung ADBP, poller, dnsfwd or display task is never detected.
- `CONFIG_LV_USE_ASSERT_MALLOC=y` with the default `while(1)` handler: the 2026-07-09 screen-freeze mechanism is still armed.
- `CONFIG_ESP_COREDUMP_ENABLE_TO_NONE`: panics leave no evidence.
- **Fix:**
  - Per-task heartbeats checked by a supervisor (or the task watchdog with a 60 s timeout and `PANIC=y`) that writes the reason to flog and then calls `esp_restart()`.
  - A custom `LV_ASSERT_HANDLER` that restarts.
  - A 64 KB coredump partition.
  - An RTC crash-loop counter.

### 6. The event-task stack is 2304 B, and it writes to flash through `logln` (plausible)
- `wifi_evt` → `logln` (`line[200]`, `vsnprintf`) → `flog_line` (`out[256]`, `snprintf`, `gmtime_r`, flash write or erase) comes to an estimated ~2.1–2.3 KB peak.
- **Scenario:** a stack-canary panic when an AP client joins or leaves.
- **Fix:**
  - Set `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096`.
  - Longer term, a flog writer task with a drop-on-full queue, so no caller ever writes to flash synchronously.

---

## P0: wrong position presented as valid

### 7. A frozen feed is reported fresh forever (verified)
- `poller.c:100` stamps `at_ms` on every parsed HTTP 200, whether or not the position moved.
- After 30 s unchanged, derive forces GS to 0, which turns off dead reckoning.
- **Scenario:** the portal keeps serving cached JSON when its avionics input stalls. The EFB shows the aircraft parked with validity=1 for the rest of the flight.
- **Fix:**
  - Use the time the position last *changed* as the fix time when the last GS was above ~50 kt.
  - Also use the feed's `updated_at`, which is currently ignored.

### 8. No sanity check on input lat/lon; glitch fixes are published (verified)
- `poller_sources.c:42` and `:118` use `atof`/`atol` with no checks. `""` or `"N/A"` becomes 0,0. `nan`, `inf`, 200 and `1e999` are all accepted.
- derive's teleport rejection only resets its ring. `apply_fix` still publishes the fix.
- **Fix:**
  - `strtod` with an end-pointer check, `isfinite`, `|lat| ≤ 90`, `|lon| ≤ 180`, and reject exactly 0,0.
  - Drop teleports as parse failures.

### 9. Dead-reckoned longitude is not wrapped at ±180 (verified)
- `adbp_frame.c:38-40`: `norm180()` exists but is not applied to `lon2`.
- Reproduced: 179.95E eastbound at 480 kt for 30 s gives **180.0176**, with validity=1.
- Dead reckoning also runs in the FRESH state, so **every antimeridian crossing** (NOU–PPT, NOU–Wallis, NOU–NRT routings) sends out-of-range GPSLONG.
- **Fix:** `p->lon = norm180(...)`, plus a host test for `adbp_dead_reckon`.

### 10. Unknown altitude is sent as FL310, valid (verified)
- `adbp_frame.c:117`: any `alt_ft ≤ 1` becomes 31000.
- Viasat with no altitude field gives 0, so on the ground the EFB is told FL310.
- **Fix:** add a `have_alt` flag and send NCD when altitude is unknown.

### 11. Quality and integrity values are fixed and contradict the position state (verified)
- `adbp_frame.c:119-130`:
  - FOM 8.0, HDOP 0.8 and HIL 0.10 NM are sent with validity=1 even when lat/lon are NCD.
  - HIL stays 0.10 NM after minutes of dead reckoning.
  - The real GPS HDOP is ignored.
  - WOW and DOOR are hard-wired "airborne, valid".
- **Fix:**
  - Send NCD with the position.
  - Grow HIL with GS × age during dead reckoning.
  - Pass the real HDOP through.
  - Send WOW as NCD.

### Smaller integrity items
- Empty `groundSpeed` is parsed as 0 kt instead of absent (`poller_sources.c:42,62`).
- GPS takes over on any GGA quality above 0, including a 2D / 3-satellite / HDOP 20 window fix, with no hysteresis (`poller.c:405`). It can flap between feed and GPS, and each flip jumps about 1 NM.
- Panasonic `true_heading` is used as track (`poller_sources.c:140`), so dead reckoning steers on heading. A 10–15° crab is a large error over a 5-minute hold.
- `stale_ms` / `dr_hold_ms` are unclamped (`web.c:737`): there is a uint32 overflow, and a long hold means long straight-line extrapolation marked valid.
- GPS fix time is refreshed by GSV/GSA sentences, not only GGA (`gps.c:120`).
- Clock: any HTTP `Date` header steps the clock with no sanity check, and the ISO time zone suffix is ignored.

---

## P1: one action, or one passenger, can take the box down mid-flight

| # | Issue | Where | Fix |
|---|---|---|---|
| 12 | `/dfu` is a plain GET with no nonce; the 30-day cookie stays valid. A restored or preloaded Safari tab can send the unit to the ROM downloader, **offline for the rest of the flight**. | `web.c:1016-1029` | POST with a one-time token; refuse it while ADBP is feeding or GS is above 50 kt |
| 13 | `esc()` does not escape `'`, but fields are single-quoted. A password containing `'` is truncated on the next Save. Below 8 characters the AP silently becomes **OPEN** (`netcore.c:381`). This is also an XSS path via a scanned SSID. | `web.c:69-80` | use `web_html_esc` everywhere |
| 14 | Defaults are `AIDlink`/`88888888` and `admin`/`password`, and changing them is never forced. "Hidden SSID", "AP channel" and "Max clients" are never applied. | `config.c:48`, `aidlink_main.c:45`, `netcore.c:376-383` | per-device random secrets, forced change; apply the settings or remove them |
| 15 | The portal, ADBP and DNS listen on the **cabin STA side** too, and mDNS advertises there. A stranger could brute-force the login, fire /dfu, or take the 4 ADBP slots. | `web.c:1048`, `adbp.c:263`, `dnsfwd.c:112`, `services.c` | drop requests arriving on the STA IP; turn off mDNS on STA |
| 16 | `/save` accepts a truncated body: checkboxes after the cut read as **off** (NAPT, auth, sim…), then it saves and reboots. | `web.c:711` | reject if `off != len` |
| 17 | No semantic validation of IP, mask, pool and ports. An empty ADBP port becomes 0 and the EFB can never connect. | `web.c:689-781` | validate everything, reject the whole save |
| 18 | Scan disconnects the STA and blocks the httpd task, so the uplink and every client stall for 2–4 s. | `netcore.c:95-116` | scan without disconnecting, or disable Scan in flight |
| 19 | A password longer than 63 characters or a username longer than 23 locks you out permanently. | `web.c:656,771`, `config.h:79` | enforce the same limits at save |
| 20 | `cfg_save` failures are ignored ("Saved ✓" even when NVS is full), and the config is written from 3 tasks with no lock. | `web.c:789` etc. | check the result; add a CFG mutex |

## P1: network resilience in the cabin

| # | Issue | Where | Fix |
|---|---|---|---|
| 21 | DHCP holds only 8 leases, against a 60-address pool and 2 h leases. Crew changes plus iOS private MACs can evict **the EFB's lease**, and ADBP subscriptions are keyed by IP. | `CONFIG_LWIP_DHCPS_MAX_STATION_NUM=8` | 32 or more |
| 22 | STA reconnects instantly with no backoff, and the disconnect reason is not logged. With the IFE down, the radio scans nonstop and the SoftAP keeps leaving its channel. | `netcore.c:238-241` | 1→30 s backoff; `logln` the reason and RSSI |
| 23 | Zero-initialised STA config means a fast scan: it joins the first cabin AP heard, not the strongest, and never roams. This fits the −77 to −88 dBm seen in flight. | `netcore.c:370` | `WIFI_ALL_CHANNEL_SCAN`, `BY_SIGNAL`, an RSSI threshold |
| 24 | `s_inet_state` stays "ok" for up to ~72 s after the STA drops (it feeds self-heal and the display). | `netcore.c:239` | set `INET_NO_UPLINK` on disconnect |
| 25 | The socket budget is exactly 16/16 in the worst case. The comments claim 16 is lwIP's maximum; it isn't (IDF allows up to 253). | `sdkconfig.defaults`, `poller.c:288` | `LWIP_MAX_SOCKETS=32`, `MAX_ACTIVE_TCP=32` |
| 26 | `SPIRAM_MALLOC_ALWAYSINTERNAL=16384` sends nearly every runtime malloc to internal RAM first: cJSON (parsed **twice** per poll), TLS records, the HTTP client, and every USB-NCM RX frame (`usb_ncm.c:49`). That 1 Hz churn is the likely cause of the 7 KB largest block. | `sdkconfig` | 2048–4096; parse once; NCM copies to PSRAM |
| 27 | The `iheap`/`iblk` telemetry includes the 32 KB `RESERVE_INTERNAL` pool that `malloc` cannot use, so the real headroom is smaller than logged. | `poller.c` | also log `MALLOC_CAP_DEFAULT\|INTERNAL` free, largest and min |
| 28 | dnsfwd accepts upstream replies without checking source or question, and the sid is predictable, so a forged reply can be cached for 300 s. With no upstream it stays silent instead of answering SERVFAIL. | `dnsfwd.c:157,183-196` | check source and question; SERVFAIL fast |
| 29 | About 135 KB of internal `.bss` could move to PSRAM: log ring 18 KB, ADBP buffers 22 KB, dnsfwd 12 KB, flog 8 KB, poller 6 KB… | see memory notes | `heap_caps_malloc(SPIRAM)` at init |
| 30 | `CONFIG_LWIP_UDP_RECVMBOX_SIZE=6` (the minimum) can drop DNS bursts; the NAPT port range overlaps lwIP's own ephemeral range. | `sdkconfig`, root `CMakeLists.txt` | mbox 16–32; disjoint NAPT range |

## P2: low risk / hygiene
- Touch (new today):
  - No re-init if the CST816 resets mid-flight; touch then stays dead until reboot. Harmless otherwise.
  - A stuck I²C bus only slows the button loop; the physical key is still checked first.
  - Fix: after N failures, pulse RST and re-send auto-sleep-off.
- `button.c:28` `now_ms() >= s_ovl_until` is not wrap-safe (49.7 days).
- `arbitrate()` reads `t` before `gps_get()`, so on dual core it can flip the source for one cycle.
- `xTaskCreate(reboot_task)` result is unchecked: config saved, but no reboot if RAM is short.
- `/flog?clear=1`, `/logout` and `/logtoggle` are side-effect GETs; erasing flog blocks `logln` for seconds.
- Feed identity strings are not escaped in HTML or in the `/status` JSON.
- TLS server certificates are not verified (`ESP_TLS_SKIP_SERVER_CERT_VERIFY`).
- Flash-log write amplification: every ADBP request and every 404 is a synchronous flash write. At 1 Hz `getParameters`, the 512 KB ring holds only ~2 h.
- No stack high-water marks are logged. `inetprobe` (3 KB, runs the HTTP client) and `button` (3 KB, runs NVS) are tight.

## Verified sound
- `pos.c` and `gps.c` are mutex-protected, so the EFB cannot receive a torn position.
- cJSON, HTTP client and socket lifetimes are paired: no leak found.
- NMEA checksum is enforced, and oversized lines are dropped.
- The dnsfwd parser is bounded.
- Antimeridian handling is correct in `geo.c`, `derive.c`, `tzdb` and `gc_slerp` (only finding 9 is broken).
- The flog ring survives power loss: the append point is the first 0xFF byte, and an interrupted erase is ignored.
- NVS is per-key with defaults, so firmware upgrades never misread config.
- Panic → reboot with 0 s delay; brownout reset is on; the interrupt watchdog is 300 ms.
- TLS can now spill to PSRAM (`MBEDTLS_DEFAULT_MEM_ALLOC`): the 09-19 handshake starvation is mitigated.

## Live test, Board 6 over USB-NCM (no uplink configured)

| Test | Result |
|---|---|
| Baseline internal heap | 17–19 KB free, 8 KB minimum, 7–7.5 KB largest block |
| 3 × 20 parallel `GET /` | 60/60 OK, but serialised: the last waited about 20 s (single httpd task) |
| 8 idle TCP connections to :80, held 20 s | `/status` still answered instantly (LRU purge works) |
| 400-query DNS burst | 0 replies (silent with no upstream, see 28); device stayed responsive |
| Idle heap trend over the following minutes | flat at ~18 KB; the earlier dip lined up with the load test, so no leak |
| Reboots / panics during the test | none (uptime continuous) |

## Suggested fix order
1. Before committing the self-heal: #3.
2. Small, contained, highest value: #1, #2, #4, #9, #10, #6 (stack size), #21, #24.
3. Next batch: #5 (supervisor + LVGL assert + coredump), #7, #8, #11, #12, #13, #16.
4. Hardening: #14, #15, #22, #23, #25–#28.
