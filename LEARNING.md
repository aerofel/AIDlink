# AIDlink — Learning Journal

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
  case. Fix direction: probe with an HTTP `generate_204` GET (hostname, ~12 s timeout for
  satellite RTT) instead of a direct-IP:53 handshake — the "content-validated alternative"
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
