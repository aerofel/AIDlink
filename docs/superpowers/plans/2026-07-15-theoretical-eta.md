# Theoretical-Profile ETA Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** FMS-steady ETA + TOD on the T-Display, computed from a theoretical
aircraft profile (Offto DB parity) with statistical winds and a slight
achieved-cruise bias; plus portal aircraft/winds settings, feed pre-selection,
yellow DB-type on line 1, IATA route display.

**Architecture:** Build-time codegen (`gen_perfdb.py`) turns the read-only
Offto SQLite into `perfdb_data.c` (30 aircraft + seasonal 250/300 hPa wind
grids). Pure modules `perfdb.c` (lookups) and `eta_profile.c` (profile,
wind triangle, bias, smoothing) are host-tested; `display.c` dispatches
profile-vs-legacy ETA; config/web/poller carry the two new settings.

**Tech Stack:** ESP-IDF 5.3.5 (targets esp32 + esp32s3), LVGL 9, Python 3 +
sqlite3 stdlib for codegen, host tests via plain `clang` (see
`firmware-idf/README.md:139`).

**Spec:** `docs/superpowers/specs/2026-07-15-theoretical-eta-design.md`

## Global Constraints

- Offto DB is read-only: open `file:...offto.sqlite?mode=ro`; canonical path
  `/Users/looping/Sites/Swift/Offto/Resources/offto.sqlite`.
- NVS keys ≤15 chars. New keys: `perf_type`, `winds_en`.
- Portal checkboxes parse by presence: `c->x = fld(body,"key",v,sizeof v);`
  never `if`-guarded (unchecked boxes are absent from the POST body).
- Both targets must build; classic esp32 has no display but perfdb/eta_profile
  compile everywhere (pure C).
- Host tests: standalone `main()` + `assert()`, compiled with the one-liner
  clang pattern; every new suite gets a line in `firmware-idf/README.md`.
- All new pure modules: no ESP-IDF includes (host-testable).
- Commit after each task. Constants from spec: bias clamp [0.90,1.10],
  bias EMA τ=600 s, output EMA τ=60 s, minute hysteresis 90 s, resync 1800 s,
  jump guard 700 kt + 25 NM, min GS 80 kt, approach reserve 60 NM,
  descent dist = ceiling/300, floor 0.8×total/cruise_TAS.

---

### Task 1: perfdb codegen + lookup module

**Files:**
- Create: `tools/gen_perfdb.py`
- Create: `firmware-idf/main/perfdb_data.c` (generated, committed)
- Create: `firmware-idf/main/perfdb.h`, `firmware-idf/main/perfdb.c`
- Create: `firmware-idf/host_test/test_perfdb.c`
- Modify: `firmware-idf/main/CMakeLists.txt:3-9` (add perfdb.c perfdb_data.c)
- Modify: `firmware-idf/README.md` (host-test section, add clang line)

**Interfaces (Produces):**
```c
// perfdb.h  (pure, host-testable)
typedef struct {
    const char *make, *model;
    char     type[5];         // ICAO type code, lookup key
    uint16_t cruise_kt;       // cruise TAS
    float    climb1_min, climb2_min, climb3_min;   // sfc→FL100, FL100→200, →TOC
    float    climb_mach;
    uint32_t ceiling_ft;
} perf_ac_t;
int perfdb_count(void);
const perf_ac_t *perfdb_get(int i);
// exact type-code match first, then exact model match (case-insensitive);
// NULL when nothing matches
const perf_ac_t *perfdb_find(const char *type_or_model);
// seasonal 250 hPa climatology at position; month 1..12; u east+ / v north+ m/s
void perfdb_wind(double lat, double lon, int month, double *u_ms, double *v_ms);
```
Data in `perfdb_data.c`: `const perf_ac_t PERFDB_AC[]`, `const int PERFDB_NAC`,
`const int8_t PERFDB_U250[4][37][6]`, `PERFDB_V250[4][37][6]`, plus
`PERFDB_U300/V300` (reserved). Season index 0=DJF 1=MAM 2=JJA 3=SON.
Lat band i: `lat_min = -90 + 5*i`, row matches `lat_min <= lat < lat_min+5`
(clamp i to 0..36). Lon sector j: normalize lon to [-180,180), `j=(lon+180)/60`
clamp 0..5. Month→season: 12,1,2→0; 3,4,5→1; 6,7,8→2; 9,10,11→3.

**Steps:**
- [ ] Write `tools/gen_perfdb.py`: argparse `--db` (default the canonical path),
  `--out` (default `<repo>/firmware-idf/main/perfdb_data.c` derived from
  `__file__` — do NOT hardcode an absolute path, gen_tzdb.py's stale Desktop
  path is the counterexample). Open read-only URI. Emit aircraft sorted by
  (make, model) and the four int8 grids (`round()` of u/v means; range check
  |v|≤127 with a hard error otherwise). Header comment: generator name, source
  DB path, generation date, row/box counts.
- [ ] Run it; eyeball `perfdb_data.c` (30 aircraft; A339 row = 460 kt,
  6.5/9/25 min, M0.82, 41000 ft; DJF 250 hPa lat-band 35..40 lon 120..180
  u≈57 — the strong winter Pacific jet).
- [ ] Write `perfdb.c` (lookups above, `strcasecmp`) and `test_perfdb.c`:
  - `perfdb_find("A339")->cruise_kt==460`, find("a339") same row,
    `find("A330-900")` same row, `find("ZZZZ")==NULL`, `find("")==NULL`,
    `find(NULL)==NULL`.
  - `perfdb_count()==30`.
  - Wind: `perfdb_wind(37, 150, 1, &u,&v)` → u≈57±1 (DJF, band 35, sector
    120..180); July same box → JJA value; lat −90 and +90 don't crash (clamp);
    lon 185 wraps to −175's sector; u/v signs match DB samples
    (lat −25 lon −120 DJF: u≈11, v≈−5).
- [ ] Run: `clang -Ifirmware-idf/main -o /tmp/t firmware-idf/host_test/test_perfdb.c firmware-idf/main/perfdb.c firmware-idf/main/perfdb_data.c -lm && /tmp/t` → PASS.
- [ ] Add both files to CMake `srcs`, add README clang line, build check later
  (Task 6 builds both targets).
- [ ] Commit `feat(idf): aircraft performance + wind climatology DB (generated from Offto, read-only)`.

### Task 2: airport elevations + IATA accessor

**Files:**
- Modify: `firmware-idf/main/airports.c` (struct + all rows + new fns)
- Modify: `firmware-idf/main/airports.h`
- Create: `firmware-idf/host_test/test_airports.c`; README line.

**Interfaces (Produces):**
```c
// airports.h additions
bool airports_lookup_ex(const char *code, double *lat, double *lon, int *elev_ft);
const char *airports_iata(const char *code);   // NULL when unknown/absent
```

**Steps:**
- [ ] `apt_t` gains `int16_t elev_ft`; fill all rows from the Offto DB values
  (already extracted): NWWW 52, NWWM 10, NWWE 315, NWWL 92, NWWR 141, NWWV 23,
  NWWA (no DB row → 0), NWWD 23, NWWU 10, YSSY 21, YBBN 13, YMML 434, YBCS 10,
  YBCG 21, YPPH 67, YPAD 20, NZAA 23, NZCH 123, NZWN 41, NFFN 59, NFNA 17,
  NTAA 5, NCRG 19, NVVV 70, NVSS 184, NLWW 79, NLWF 20, NFTF 126, NSFA 58,
  NSTU 32, AGGH 28, AYPY 146, PGUM 298, PHNL 13, RJAA 141, RJTT 35, RJBB 26,
  RJGG 15, RKSI 23, WSSS 22, VTBS 5, VHHH 28, ZSPD 13, WADD 14, KLAX 125,
  KSFO 13, LFPG 392.
- [ ] Implement `airports_lookup_ex` (elev out-param optional) and
  `airports_iata` (returns `a->iata` or NULL). `airports_lookup` stays as-is.
- [ ] `test_airports.c`: NWWW→elev 52 + iata "NOU"; lookup by IATA "NRT" gives
  same row as "RJAA" (elev 141); unknown code → false/NULL. Run clang line → PASS.
- [ ] Commit `feat(idf): airport elevations + IATA accessor in the gazetteer`.

### Task 3: eta_profile pure engine + made-good accessor

**Files:**
- Create: `firmware-idf/main/eta_profile.h`, `firmware-idf/main/eta_profile.c`
- Modify: `firmware-idf/main/eta.h`, `firmware-idf/main/eta.c` (accessor)
- Create: `firmware-idf/host_test/test_eta_profile.c`; README line.
- Modify: `firmware-idf/main/CMakeLists.txt` (add eta_profile.c)

**Interfaces (Consumes):** `perfdb_find/perfdb_wind` (Task 1),
`geo_dist_nm/geo_bearing_deg` (existing), `eta_state_t` ring (existing).

**Interfaces (Produces):**
```c
// eta.h addition
// Window made-good ground speed from the sample ring (kt), or -1 while the
// ring spans under ETA_MIN_SPAN_S.
double eta_made_good_kt(const eta_state_t *st);

// eta_profile.h
typedef struct {
    double r_ema; bool r_init;         // cruise bias ratio (1.0 = on-profile)
    bool   have_eta;
    double eta_s, tod_s;               // smoothed epochs (s)
    double last_s, last_dist, last_now;
    long   shown_eta_min, shown_tod_min;
} etap_state_t;
typedef struct { long eta_min, tod_min; } etap_out_t;   // 0 = none / TOD passed
void etap_reset(etap_state_t *st);
etap_out_t etap_update(etap_state_t *st, const perf_ac_t *ac,
                       double lat, double lon,          // current position
                       double alat, double alon,        // arrival
                       int dest_elev_ft,
                       double tot_nm, double dist_to_go_nm,
                       double gs_inst_kt, double gs_made_good_kt,
                       double now_s, int month, bool winds);
```

**Algorithm (all formulas from the spec §5, Offto parity):**
1. Guards: `dist<0 || now<=0` → return {0,0}, keep state. Same jump/clock-back
   reset as eta.c (`>700kt·dt+25NM` on dist_to_go, `now < last_now−1`).
   Flying gate: `max(gs_inst, gs_made_good) < 80` → `{0,0}`, keep state.
2. `covered = clamp(tot − dist_to_go, 0, tot)`.
   `climb_d/t` from the 3 segments (280 / 380 / mach×593.7 kt);
   `desc_d = ceiling/300`; `cruise_d = max(0, tot − climb_d − desc_d − 60)`.
3. Build a monotonic breakpoint table `(dist_from_origin, cum_time_s)`:
   climb sub-segment ends; cruise split into ≤24 wind segments of ≤200 NM
   (geometry: great-circle slerp between current pos and arr, segment course =
   bearing(mid→arr), wind = perfdb_wind at mid, GS = wind triangle, winds off →
   GS = cruise TAS); descent as 40 integration steps (IAS keyed on height above
   dest: >10000→290, lerp 10000:250 / 4000:180 / 0:140; TAS = IAS×(288.15 /
   max(288.15−0.0019812·alt_msl, 216.65))^2.128); approach stages 60/40/25/15/8
   at 390/280/220/180/140 kt TAS (+arrival wind when winds on).
   Wind triangle: `swc=(W/TAS)sin(Δ)`; `|swc|>1 → max(TAS/2,50)`;
   else `TAS·√(1−swc²) − W·cos(Δ)`, floor 50 kt. u/v→met: `spd=hypot·1.944`,
   `dir=atan2(−u,−v)`.
4. Bias: if `covered` inside cruise and `gs_made_good ≥ 80`:
   `r_raw = gs_made_good / gs_theo(segment containing covered)`; EMA τ=600 s
   into `r_ema`, clamp [0.90, 1.10]. `p = (covered−climb_d)/cruise_d` clamp
   0..1; remaining cruise segment speeds ×= `(1 + (r_ema−1)·p)` before the
   table is built (climb/descent/approach never biased).
5. Floor: `T_total ≥ 0.8×tot/cruise_kt` → scale all cum times up if under.
6. `raw_eta = now + (T_total − interp(covered))`;
   `raw_tod = now + (interp(climb_d+cruise_d) − interp(covered))` while
   `covered < climb_d+cruise_d`, else TOD passed.
7. Conditioning: first estimate or `|raw−ema|>1800` → snap; else EMA τ=60 s;
   displayed minute moves only past ±90 s (independently for eta and tod).
   TOD passed → `tod_min=0` (and stays 0).

**Steps:**
- [ ] Add `eta_made_good_kt()` to eta.c (ring oldest→newest Δdist/Δt·3600,
  −1 if span<ETA_MIN_SPAN_S) + 2 asserts in existing `host_test/test_eta.c`
  (returns ≈480 on the steady-flight scenario; −1 right after reset). Run
  test_eta clang line → PASS.
- [ ] Write `eta_profile.c` per the algorithm; internal helpers `static`:
  `ias_to_tas()`, `wind_gs()`, `gc_slerp()` (standard great-circle
  interpolation), `descent_time()`, `approach_time()`, `build_table()`.
- [ ] Write `test_eta_profile.c` (deterministic, no RNG):
  - **Phase math (A339, no wind, dest elev 0):** climb_t=2430 s exactly;
    climb_d≈290.2±0.5 NM; desc_d≈136.67±0.01; approach exactly 886.8±2 s;
    descent_time within [1200,2000] s and strictly larger for dest_elev=0 vs
    5400 ft (more air to descend through → longer).
  - **IAS→TAS:** 250 kt @10000 ft → 289±2 kt; 290 @39000 → 490±10.
  - **Wind triangle:** TAS 460, direct 60 kt headwind → 400; tailwind → 520;
    90° cross 60 kt → ≈456.
  - **Steadiness (the point of it all):** simulate NWWW→RJAA A339: position
    advances along the GC at gs(t)=460+25·sin(2πt/1200) kt (winds OFF so truth
    is known), 5 s steps, feeding both `eta_update` (for made-good) and
    `etap_update`. Assert: profile ETA appears within 60 s of gate-open;
    displayed eta_min changes ≤3 distinct values from t=15 min to end of
    cruise; TOD minute changes ≤2 values and goes 0 after covered passes
    climb_d+cruise_d; final displayed ETA within 3 min of the simulation's
    true arrival.
  - **Bias:** same sim but aircraft actually flies 0.96×theory: assert r_ema
    < 1 after 20 min cruise, and abs(displayed − true arrival) shrinks
    monotonically sampled at 25/50/75% cruise; correction at p=0.1 is small
    (< 25% of correction at p=0.9).
  - **Resets:** dest change (dist_to_go +800 NM) resets (eta blanks then
    re-appears); NCD (`dist=−1`) for 60 s keeps state (same minute after).
  - **Winds:** with winds ON and the DJF Pacific jet, NRT→NOU (southbound
    ≈ headwind-ish component) remaining time > winds-OFF time for the same
    geometry — sanity sign check only.
- [ ] Run: `clang -Ifirmware-idf/main -o /tmp/t firmware-idf/host_test/test_eta_profile.c firmware-idf/main/eta_profile.c firmware-idf/main/perfdb.c firmware-idf/main/perfdb_data.c firmware-idf/main/eta.c firmware-idf/main/geo.c -lm && /tmp/t` → PASS.
- [ ] CMake + README lines. Commit `feat(idf): theoretical-profile ETA engine (host-tested)`.

### Task 4: config + portal card + feed pre-selection

**Files:**
- Modify: `firmware-idf/main/config.h:49` area (two fields),
  `config.c` (~line 62 defaults, ~104 load, ~153 save)
- Modify: `firmware-idf/main/web.c` (card after ⑤ Emulator ~line 392; h_save
  ~line 585; small `<script>` with the filter JS near the existing srcSel())
- Modify: `firmware-idf/main/poller.c:70-74` area (pre-select after ac_type)

**Interfaces (Consumes):** `perfdb_count/get/find` (Task 1).

**Steps:**
- [ ] config.h: after `ac_tail/ac_type` add
  `char perf_type[8]; bool winds_enable;` (comment: persisted, unlike identity).
  config.c: defaults `c->perf_type[0]=0; c->winds_enable=true;`;
  load `get_str(h,"perf_type",...)`, `c->winds_enable=get_bool(h,"winds_en",...)`;
  save `nvs_set_str(h,"perf_type",...)`, `nvs_set_u8(h,"winds_en",...)`.
- [ ] web.c card (between Emulator and Security), exact shape:
  ```c
  chunk(r, "<div class='card'><h2>✈ ETA — aircraft performance</h2><div class='grid'>");
  const perf_ac_t *cur = perfdb_find(c->perf_type);
  chunk(r, "<div class='f'><label>Manufacturer</label><select id='perfMake' onchange='perfSel()'><option value=''>—</option>");
  for (int i = 0; i < perfdb_count(); i++) {          // unique makes, in order
      const perf_ac_t *a = perfdb_get(i);
      if (i && !strcmp(a->make, perfdb_get(i-1)->make)) continue;
      chunkf(r, "<option value='%s'%s>%s</option>", a->make,
             cur && !strcmp(cur->make, a->make) ? " selected" : "", a->make);
  }
  chunk(r, "</select></div><div class='f'><label>Aircraft type</label><select id='perfType' name='perfType'><option value=''>— (GS estimator)</option>");
  for (int i = 0; i < perfdb_count(); i++) {
      const perf_ac_t *a = perfdb_get(i);
      chunkf(r, "<option value='%s' data-make='%s'%s>%s (%s)</option>", a->type,
             a->make, cur == a ? " selected" : "", a->model, a->type);
  }
  chunk(r, "</select></div>");
  ff_tog(r, "Statistical winds (position)", "windsEn", c->winds_enable);
  chunk(r, "</div><div class='note'>Theoretical ETA/TOD from the selected profile (climb/cruise/descent + seasonal winds aloft). The live feed auto-selects a matching type. “—” keeps the plain ground-speed estimator.</div></div>");
  ```
  JS (append inside the existing script block): `perfSel()` hides
  `#perfType option`s whose `data-make` ≠ chosen make (empty make shows all)
  and resets the value if the current option got hidden; on load, derive
  `perfMake` from the selected type option's `data-make` (server already marks
  both selected, so this is only a JS-consistency no-op on load).
- [ ] h_save: **first check `web_util.c:75` `web_form_field` semantics for
  empty values** (`perfType=` with empty value must still clear the field);
  if it returns false on empty, detect key presence separately. Then:
  `if (fld(body,"perfType",v,sizeof v)) { strlcpy(c->perf_type, v, sizeof c->perf_type); if (!perfdb_find(c->perf_type)) c->perf_type[0]=0; }`
  plus empty-value clear, and `c->winds_enable = fld(body,"windsEn",v,sizeof v);`
  (presence rule, unguarded).
- [ ] poller.c apply_fix, after the ac_type block:
  ```c
  // A feed-supplied type pre-selects the performance profile: exact DB match
  // only (type code, then model), persisted once per change so it survives
  // reboots even though the identity itself is RAM-only.
  if (!sim && actype && actype[0]) {
      const perf_ac_t *pa = perfdb_find(actype);
      if (pa && strcmp(pa->type, CFG->perf_type) != 0) {
          strlcpy(CFG->perf_type, pa->type, sizeof CFG->perf_type);
          cfg_save(CFG);
      }
  }
  ```
- [ ] Build esp32s3 target (`idf.py build`) — no flash yet. Commit
  `feat(idf): ETA/performance settings card, winds switch, feed profile pre-select`.

### Task 5: display integration (dispatch, TOD, yellow type, IATA)

**Files:**
- Modify: `firmware-idf/main/display.c` (statics ~95-104, build_ui ~242-309,
  refresh ~383-532)

**Interfaces (Consumes):** `etap_update/etap_reset` + `eta_made_good_kt`
(Task 3), `perfdb_find` (Task 1), `airports_lookup_ex/airports_iata` (Task 2),
`CFG->perf_type/winds_enable` (Task 4).

**Steps:**
- [ ] Statics: `static etap_state_t s_etap;` `static lv_obj_t *sg_tod;`
  `static lv_span_t *sp_tod_l, *sp_tod_t, *sp_tod_z;`
- [ ] build_ui: `s_actype` color → `COL_YELLOW` (line 243). Move `sg_eta` to
  `LV_ALIGN_BOTTOM_MID, 48, -34`; add
  `sg_tod = mkspangroup(scr, LV_ALIGN_BOTTOM_MID, -48, -34);`
  `sp_tod_l = addspan(sg_tod, &lv_font_montserrat_14, COL_GREY);`
  `sp_tod_t = addspan(sg_tod, &lv_font_montserrat_16, COL_AMBER);`
  `sp_tod_z = addspan(sg_tod, &lv_font_montserrat_14, COL_GREY);`
  (±48 offsets are the first guess; verify on hardware that
  dist | TOD | ETA | UTC+11 don't collide at 320 px and tune.)
- [ ] refresh() route block (lines 411-417): prefer IATA —
  ```c
  const char *o = airports_iata(p.orig); if (!o) o = airports_icao(p.orig);
  if (!o) o = p.orig[0] ? p.orig : "----";   // same for dest
  ```
- [ ] refresh() type slot (line 394): resolved DB code only, never feed string:
  ```c
  const perf_ac_t *pa = perfdb_find(CFG->perf_type);
  lv_label_set_text(s_actype, pa ? pa->type : "");
  ```
- [ ] refresh() needs dest elevation: switch the dist lookup (line 448) to
  `airports_lookup_ex(p.dest, &alat, &alon, &dest_elev)`.
- [ ] ETA dispatch (replace lines 520-532): legacy `eta_update` always runs
  (ring + fallback); when `pa && dist_nm>=0 && tot_nm>10 && utc`:
  ```c
  struct tm tmu; time_t us = (time_t)(utc/1000); gmtime_r(&us, &tmu);
  etap_out_t po = etap_update(&s_etap, pa, p.lat, p.lon, alat, alon, dest_elev,
                              tot_nm, dist_nm, p.gs_kt,
                              eta_made_good_kt(&s_eta),
                              utc/1000.0, tmu.tm_mon + 1, CFG->winds_enable);
  if (po.eta_min > 0) { eta_min = po.eta_min; tod_min = po.tod_min; }
  ```
  Render `tod_min>0` as `sp_tod_l="TOD "`, `sp_tod_t="HH:MM"`, `sp_tod_z="z"`,
  else all three empty. ETA rendering unchanged (it just may now carry the
  profile value).
- [ ] Build esp32s3. Commit `feat(idf): profile ETA + TOD on display, yellow DB type, IATA route`.

### Task 6: dual-target build, all host tests, flash, verify, docs

**Steps:**
- [ ] All host suites (README one-liners): existing 6 + test_perfdb,
  test_airports, test_eta_profile → all PASS.
- [ ] `idf.py build` (esp32s3, default build dir) and
  `idf.py -B build-esp32 -DIDF_TARGET=esp32 -DSDKCONFIG=$PWD/build-esp32/sdkconfig build`
  (LEARNING.md dual-target pattern) → both clean; check app size headroom.
- [ ] Flash the T-Display: portal `/dfu` (auth per memory) with
  `tools/autoflash-idf.sh` waiting, or physical BOOT+RST fallback; post-flash
  RST tap ritual. Verify boot via `/log` over the cable (`curl --interface`
  gotcha), `/status` sane.
- [ ] Bench-verify with a moving feed (scratchpad flightsrv.py pattern,
  NWWW→RJAA): portal shows the new card; select Aircalin/A339; display shows
  yellow `A339`, `NOU➤NRT`, TOD + steady ETA; toggle winds and watch the ETA
  step once and re-steady.
- [ ] LEARNING.md entry (top): profile-ETA design decisions + any new gotchas.
  CHANGELOG if present (append at top per global rules). Final commit.

## Self-review notes

- Spec §3 codegen→Task 1; §3 elevations/IATA→Task 2; §5 engine+§5.3 TOD→Task 3;
  §4 settings+pre-select→Task 4; §6 display (dispatch/TOD/yellow/IATA)→Task 5;
  §8 tests→Tasks 1/2/3; §9 budget checked in Task 6 (build sizes). No gaps.
- Names cross-checked: `perf_ac_t`, `perfdb_find`, `perfdb_wind`,
  `eta_made_good_kt`, `etap_state_t/etap_update/etap_out_t`,
  `airports_lookup_ex/airports_iata`, cfg `perf_type/winds_enable`,
  NVS `perf_type/winds_en`, form `perfType/windsEn` — consistent across tasks.
- Known judgment calls at execution: `web_form_field` empty-value semantics
  (Task 4 step explicitly verifies), TOD/ETA ±48 px offsets (hardware tune).
