# Theoretical-Profile ETA (FMS-steady) — Design

Date: 2026-07-15 · Status: awaiting user approval · Target: `firmware-idf/`

## 1. Problem

The displayed ETA (eta.c v3, made-good ground speed over a 600 s window + EMA +
minute hysteresis) still moves too much in flight. A real FMS stays steady from
takeoff to end of cruise because it predicts the *whole remaining flight* from a
wind-corrected performance profile and only nudges that prediction with small
measured deviations. Goal: reproduce that behavior — ETA shown shortly after
takeoff ≈ ETA at end of cruise, moving by low single-digit minutes.

## 2. Approaches considered

- **A (chosen) — Full theoretical profile engine**, ported from Offto's
  `FlightProfileCalculator.swift` (documented in `FLIGHT_TIME_CALCULATION.md`):
  climb/cruise/descent/approach segments from the aircraft DB + seasonal
  statistical winds, with a small measured cruise bias. Deterministic, steady
  by construction; the made-good estimator stays as fallback.
- **B (rejected) — More damping on the existing estimator**: cannot be steady
  *and* honest — slow oscillations pass any filter (proven by eta v1), and it
  ignores the known future (descent slowdown), so it drifts late in cruise.
- **C (rejected) — Embed SQLite + ship offto.sqlite in flash**: ~250 KB code +
  9.2 MB DB (with image blobs), needs a FS partition; absurd for 30 rows.

## 3. Data pipeline — single source of truth

- Canonical DB: `/Users/looping/Sites/Swift/Offto/Resources/offto.sqlite`
  (the root-level `offto.sqlite` is a 0-byte stub — never use it). Opened
  **read-only** (`file:...?mode=ro`) at *generation time only*; never modified,
  never copied into this repo. No second database to maintain.
- New generator `tools/gen_perfdb.py` (tzdb pattern: emits a committed C file,
  listed in `main/CMakeLists.txt`; header comment records source path + date;
  re-run manually when the Offto DB changes):
  - `perfdb_data.c`:
    - `PERF_AC[]` (30 rows): `make, type (ICAO code, key), model,
      cruise_tas_kt (=speed), climb_to_fl100_min, climb_fl100_fl200_min,
      climb_above_fl200_min, climb_mach, ceiling_ft`. ≈1.2 KB rodata.
    - Wind climatology 250 hPa: `int8_t WIND_U/V[4 seasons][37 lat][6 lon]`
      in m/s (ERA5 means fit int8). 300 hPa emitted too (reserved, unused in
      v1). ≈3.6 KB rodata total.
- **Airport elevations + IATA**: `apt_t` in `airports.c` gains
  `int16_t elev_ft`. Values for the ~44 gazetteer rows come from `airportslt`
  (script prints `ICAO → elev, iata` for hand-merge; the gazetteer stays
  hand-maintained per its own comment). Missing elevation → 0 (sea level).
  The script also flags gazetteer rows whose IATA is missing/mismatched vs
  the Offto DB.

## 4. Settings (config + portal)

- `aidlink_cfg_t` additions (config.h + one `get_*`/`nvs_set_*` line each):
  - `char perf_type[8]` — selected aircraft ICAO type code. NVS `perfType`.
    Default `""` = no profile = legacy ETA.
  - `bool winds_enable` — statistical winds by position. NVS `windsEn`.
    Default **on** (only meaningful when a profile is active).
- Portal: new card **"ETA / Performance"**:
  - `Manufacturer` select + `Aircraft type` select. Options generated from
    `PERF_AC` (`<option data-make="Airbus" value="A339">A330-900 (A339)</option>`);
    ~15 lines of inline JS filter the type list by the chosen make and set the
    make select from the persisted type on load. Only the type code is
    submitted/persisted (`perfType`); make is derivable.
  - Checkbox `Statistical winds (position)` → parsed by **presence**
    (`c->winds_enable = fld(body,"windsEn",v,sizeof v);` — never `if`-guarded).
- **Feed pre-selection**: when the position feed supplies an aircraft type
  string, try to match a `PERF_AC` row — exact `type` match first, then exact
  `model` match (both case-insensitive). On match, set + persist `perf_type`
  (once per change, in `apply_fix`, like identity used to persist). The portal
  then shows it selected; a manual portal choice still works but a later feed
  match overwrites it. No match → no change (garbage can't select anything).

## 5. ETA engine v4 — new pure module `eta_profile.c/h`

Inputs per refresh (all already available at the single call site in
display.c): `dist_to_go` NM (pos→arr great-circle), `total` NM (dep→arr),
`gs_made_good_kt` (new accessor `eta_made_good_kt()` exposing the existing
600 s ring), `now` epoch s, month (for season), current lat/lon, arr lat/lon +
`elev_ft`, the resolved `PERF_AC` row, `winds_enable`.

### 5.1 Theoretical profile (formulas = Offto parity)

- **Climb** — 3 segments, times straight from DB minutes, distances at fixed
  average speeds: below FL100 `280 kt`; FL100→FL200 `380 kt`; FL200→TOC
  `climb_mach × 593.7 kt` (speed of sound at FL280 ISA).
- **Descent** — distance `ceiling_ft / 300` NM (kept coarse deliberately —
  stability over precision, per Offto). Time = 40-step integration mapping
  distance linearly from `ceiling` MSL down to destination `elev` MSL; at each
  step midpoint: IAS schedule keyed on **height above destination**:
  `>10 000 ft → 290 · 10 000 → 250 · 4 000 → 180 · 0 → 140` (linear between
  points; this is the 250 kt-at-10 000-ft-above-airport rule); IAS→TAS at the
  true MSL altitude: `TAS = IAS × (288.15 / max(288.15 − 0.0019812·alt_ft,
  216.65))^2.128`.
- **Approach** — final 60 NM staged by remaining distance:
  `<60→390, <40→280, <25→220, <15→180, <8→140 kt` (treated as TAS; single
  arrival wind applied when winds on — Offto parity).
- **Cruise** — `cruise_d = max(0, total − climb_d − desc_d − 60)` at
  `cruise_tas_kt`. Winds on: split the cruise portion of the great-circle at
  5°-lat × 60°-lon climatology-box crossings (sampled every ~100 NM); each
  segment gets the seasonal 250 hPa wind at its midpoint (u/v → speed/dir:
  `spd = √(u²+v²)×1.944`, `dir = atan2(−u,−v)` normalized) and the standard
  wind-triangle GS: `swc = (W/TAS)·sin(dir−crs)`;
  `GS = TAS·√(1−swc²) − W·cos(dir−crs)`, floors: `|swc|>1 → max(TAS/2, 50)`,
  else `max(GS, 50)`. Season from month (DJF/MAM/JJA/SON).
- **Remaining time** from `covered = clamp(total − dist_to_go, 0, total)`:
  piecewise-linear interpolation over the breakpoints (cruise split per wind
  segment). Whole-profile floor `0.8 × total/cruise_tas` (Offto parity).

### 5.2 Cruise bias — the only adaptive term (primary correction stays the profile)

Active only while `covered` lies inside the theoretical cruise segment.
`p = (covered − climb_d)/cruise_d` clamped 0..1 (fraction of cruise achieved).
Measurement: `r_raw = gs_made_good / gs_theo(current wind segment)`; EMA with
τ = 600 s; clamp `r ∈ [0.90, 1.10]`. Applied correction: every remaining
cruise segment's GS ×= `(1 + (r−1)·p)` — zero correction at cruise start,
full (still clamped, typically 2–3 %) by end of cruise, exactly "very slight,
scaled by percentage of cruise achieved". Climb/descent/approach are never
biased. Note: with winds OFF the bias is the only wind correction and the
clamp deliberately limits it — winds ON is the accuracy path.

### 5.3 Output conditioning + guards

`arrival_epoch = now + remaining_s`, then a light EMA (τ = 60 s) and the
displayed-minute hysteresis ±90 s (same as legacy). Reset/robustness rules
carried over from eta.c v3: distance-jump guard (>700 kt-equivalent + 25 NM
slack → full reset, incl. bias), clock-step-back → reset, NCD blip → blank
display but keep all state, GS < 80 kt → blank.

## 6. Display changes (display.c)

- **ETA source dispatch**: legacy `eta_update()` keeps running every refresh
  (maintains the ring, provides fallback). The profile ETA is shown when a
  `PERF_AC` row is resolved (config or feed match) AND dep+arr resolve in the
  gazetteer; otherwise the legacy value is shown. Same spans, same format.
- **Aircraft type, line 1 center**: between the tail (left) and flight number
  (right), show the **resolved DB type code** (e.g. `A339`) in yellow
  (montserrat 16, `0xFFD54A`-family — final shade tuned on hardware next to
  the existing amber). This replaces the feed-string type in that slot; the
  feed string is never displayed, it only drives DB matching (§4). No resolved
  profile → the slot is blank.
- **IATA route display**: wherever dep→arr codes are *displayed* (route line,
  splash/trip contexts), show the gazetteer row's IATA when non-empty, else
  ICAO (e.g. `NOU→NRT` instead of `NWWW→RJAA`). ADBP/`/status`/logs keep ICAO
  (protocol surfaces, not display).

## 7. Edge cases

- `dist_to_go > total` (early off-track) → `covered = 0` (profile start).
- `cruise_d ≤ 0` (short leg) → floor keeps the estimate sane and monotonic.
- Unknown destination elevation → 0 ft.
- `perf_type` no longer in a regenerated `PERF_AC` → legacy ETA + one log line.
- No clock / no fix → blank, as today. x10 bench replays → jump guards as
  today (regression-tested).

## 8. Testing (host, clang one-liner pattern + README lines)

- `test_perfdb.c` — row lookup by type/model (case-insensitive), season
  mapping, lon normalization, box edges at poles/antimeridian, u/v→dir/speed.
- `test_profile.c` — climb/descent/approach times vs hand-computed values from
  real DB rows (A339 + one bizjet); IAS→TAS spot values; wind-triangle cases
  (pure head/tail/crosswind, `|swc|>1` branch); elevation-aware descent
  (KDEN-like 5 400 ft vs sea level differ by the right sign/magnitude).
- `test_eta_profile.c` — full simulated NOU→NRT flight, ±10 kt GS wiggle +
  constant real wind: displayed ETA drifts ≤3 min from first cruise minute to
  end of cruise, final ETA within a few min of simulated arrival; bias ramps
  with `p` and converges when actual GS is 3 % off theory; NCD blip keeps
  state; destination change resets; winds on/off produce the expected split.
- Legacy `test_eta.c` unchanged and still passing.

## 9. Budget

≈5 KB rodata (aircraft + both wind levels), <300 B state, and per-refresh math
(40-step integration + ≤10 wind segments at 0.5 Hz) is negligible. No new
LVGL per-frame allocations (one extra static span) — no pool impact.

## 10. Decisions taken — flag any you disagree with

1. **Winds default ON** (`windsEn`), only active when a profile is resolved.
2. **`perf_type` default empty** → legacy ETA until a portal choice or feed
   match; feed match persists and can overwrite a manual choice.
3. **Bias clamp ±10 %**, correction scaled by cruise fraction `p`.
4. **250 hPa only** in v1 (exact iOS-app parity; the doc's "250/300 blend" is
   not what the Swift code does). 300 hPa data ships in flash, unused.
5. **Elevations/IATA hand-merged** into airports.c from script output; the
   gazetteer stays hand-maintained.
6. **Legacy eta.c untouched** — fallback + ring provider; new logic lives in
   `eta_profile.c` (pure, host-tested).

## 11. Accuracy suggestions (beyond Offto parity)

1. **The FMS-likeness driver is winds ON.** The FMS is steady because its
   prediction already contains the winds; the climatology gives us the same
   property. Expect post-takeoff ≈ end-of-cruise ETA within a few minutes.
2. **Altitude wind interpolation** (v2): blend 250/300 hPa by ceiling —
   ceilings below ~FL340 sit nearer 300 hPa; data already shipped.
3. **Actual-altitude descent detection** (v2): if measured altitude drops
   >2 000 ft below ceiling while the theoretical phase is still cruise,
   re-anchor the descent at the actual position — handles early descents/ATC
   shortcuts without disturbing steadiness in cruise.
4. **Route-stretch factor** (v2): when the flown path consistently exceeds
   great-circle progression (airways, weather deviations), a slow ratio could
   scale remaining distance; same slew/clamp discipline as the cruise bias.
5. **Validation aid**: expose `eta_profile` internals (phase, r, p, per-phase
   times) in `/status` JSON during flight testing, to compare against the FMS.
