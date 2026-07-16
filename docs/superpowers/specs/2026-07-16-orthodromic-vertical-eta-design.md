# Orthodromic Vertical ETA Design

**Date:** 2026-07-16  
**Status:** IMPLEMENTED 2026-07-16 — see section 12 for the isolation-matrix
outcome (stretch and bias multipliers ship disabled by this spec's own
acceptance rules)  
**Scope:** Generic theoretical ETA profile, constrained to origin/destination
orthodromy, with the A339 replay fleet as the first validation set

## 1. Objective

Make the displayed ETA stable throughout a flight by predicting the whole
remaining flight from a fixed theoretical profile. The estimator cannot use a
flight-plan route or live route waypoints. It may use:

- origin and destination airports;
- current position and altitude;
- an aircraft performance row;
- seasonal statistical winds;
- immutable departure-to-arrival orthodromic distance and bearing; and
- fixed statistical priors calculated before profile evaluation.

Actual-versus-statistical wind error is outside this correction. Replay must
still run with production winds, but wind discrepancies must not be hidden by
tuning unrelated performance fields.

## 2. Evidence from the current implementation

### 2.1 Replay baseline

Twelve available A339 flights were replayed through the firmware engine. The
current production configuration averages approximately:

- 49.9 displayed ETA changes per flight;
- 3.1 direction reversals;
- a 25.3-minute error span; and
- a final error near -1.1 minutes.

These values are the regression baseline, not a claim of absolute model
accuracy. Several tracks have 32-211 minute ADS-B gaps. Straight chords across
those gaps make measured flown distances conservative and can create replay-only
ETA steps.

### 2.2 Confirmed causes unrelated to daily wind

1. `ceiling_ft` is treated as both service ceiling and initial cruise level.
   The model therefore starts every flight at the ceiling and has no step
   climbs.
2. The aircraft database contains range, but `tools/gen_perfdb.py` does not
   generate it into `perf_ac_t`. The firmware cannot infer weight-related
   initial cruise altitude from trip length.
3. The DB upper-climb time is consumed as one FL200-to-ceiling segment at a
   fixed `Mach * 593.7` speed. It is not scaled to the selected initial cruise
   level and step climbs consume no time or distance.
4. Live altitude is available, and replay parses altitude, but
   `etap_update()` does not receive it. The model cannot recognize a real early
   descent.
5. The 2 Hz display calls the filters with a whole-second epoch. Duplicate
   timestamps are forced to 0.5 seconds and the following call counts a full
   second. The filters therefore integrate about 1.5 seconds per wall-clock
   second. A configured 2700-second bias behaves near 1800 seconds and the
   60-second output filter behaves near 40 seconds.
6. `eta_made_good_kt()` measures direct-to-destination closure. It is compared
   with predicted along-path ground speed. Doglegs are consequently learned as
   false TAS or wind error.
7. Descent is integrated from cruise altitude to field elevation and then a
   separate 60 NM approach is appended. The lower descent is modeled twice,
   producing an A339 nominal TOD near 197 NM while observed TOD is roughly
   99-162 NM.
8. Full model strings such as `A330-900` cannot survive the seven-character
   payload in `actype[8]`, although ICAO `A339` works. Replay bypasses this
   integration defect.

### 2.3 Aircraft database assessment

The generated A339 values exactly match the Offto database:

| Field | Current value | Assessment |
|---|---:|---|
| range | 5500 NM | Suitable first value for the range-ratio heuristic |
| cruise speed | 460 kt TAS | Correctly transcribed, but too coarse to explain every route family |
| climb 0-FL100 | 6.5 min | No evidence for a correction |
| climb FL100-FL200 | 9.0 min | No evidence for a correction |
| climb above FL200 | 25.0 min | Keep as time from FL200 to service ceiling; scale it in firmware |
| climb Mach cap | 0.82 | Correct semantic input; current constant-speed use is simplistic |
| ceiling | 41000 ft | Correct as service ceiling; wrong when used as mandatory cruise altitude |

Do not change the A339 database row merely to make the current replay errors
smaller. The observed TAS difference is route-dependent, and TAS, climatology,
route geometry, and terminal behavior currently cancel one another on some
flights.

## 3. Statistical orthodromy stretch

### 3.1 Measurement

The airborne KML polyline length was compared with the great-circle distance
between its first and last airborne points. No taxi points below 50 kt were
present.

| Direction | Samples | Median excess |
|---|---:|---:|
| LFPG to VTBS | 4 | 5.05% |
| VTBS to LFPG | 2 | 6.38% |
| VTBS to NWWW | 3 | 2.89% |
| NWWW to VTBS | 2 | 1.43% |
| YSSY to NWWW | 1 | 0.33% |

Across all 12 flights:

- median excess: 4.22%;
- distance-weighted excess: 4.19%;
- 10% trimmed mean: 3.91%; and
- interquartile range: 2.12-5.55%.

A universal 4.2% factor is therefore a defensible long-haul aggregate, but it
would incorrectly add about 45 NM to the 1061 NM SYD-NOU sector. Directional
city-pair priors would be more accurate, but the requested design is generic
and cannot depend on route data.

Coverage gaps represent 14-50% of several measured polylines as straight
chords, so these excesses are conservative lower bounds. Conversely, some
long flights contain 23-63 NM of terminal vectoring and 8-17 NM of backtracking.
The correction must consequently be bounded and treated as a provisional
network prior rather than a physical aircraft property.

### 3.2 Generic distance-only formula

Calculate the factor once from the immutable airport-to-airport orthodromic
distance:

```text
x = clamp((raw_total_gc_nm - 1500) / 3600, 0, 1)
stretch_pct = 5.5 * x * x * x * x
stretch_scale = 1 + stretch_pct / 100
```

Expected anchors are:

| Raw orthodromy | Added percentage | Approximate added distance |
|---:|---:|---:|
| 1061 NM | 0.00% | 0 NM |
| 4395 NM | 2.30% | 101 NM |
| 5101 NM | 5.50% | 281 NM |

This curve is preferred to a linear ramp because a linear ramp that reaches
5% at 5100 NM materially over-corrects the 4395 NM routes. The fourth-power
curve is deliberately provisional. More independent route families must be
collected before presenting it as a general aviation statistic.

### 3.3 Application rules

Apply the same frozen factor to both total and remaining geometry:

```text
profile_total_nm = raw_total_gc_nm * stretch_scale
profile_remaining_nm = raw_remaining_gc_nm * stretch_scale
profile_covered_nm = profile_total_nm - profile_remaining_nm
```

This preserves the raw completion fraction exactly and makes the correction
burn off continuously. The factor is recomputed only when origin or destination
changes and the ETA profile is reset.

The screen must continue to show raw orthodromic NM and raw orthodromic trip
completion. Diagnostics and replay output must expose both raw and effective
distances.

Never:

- add the excess only to total distance;
- add the excess only to remaining distance;
- recompute the percentage from current remaining distance; or
- infer it from accumulated flown distance divided by GC progress.

The last method was already replayed and rejected: front-loaded route excess
was extrapolated over the whole remaining flight and error spans reached about
93 minutes.

## 4. Fixed vertical profile

### 4.1 Inputs and initialization

Generate `max_range_nm` from the existing Offto `airplanes.range` column into
`perf_ac_t`. The source database remains read-only from this repository.

Use corrected planned distance for the range fraction:

```text
range_fraction = clamp(profile_total_nm / max_range_nm, 0, 1)
steps_below_ceiling = round(3 * range_fraction * range_fraction)
```

The range-based altitude heuristic was calibrated against flown distance, so
using `profile_total_nm` is intentional. `steps_below_ceiling` is an integer
from zero through three.

### 4.2 Direction-compatible usable ceiling

Use the initial orthodromic true bearing as the only available proxy for the
semicircular rule:

- initial course 000-179 degrees: highest odd-thousand flight level at or
  below the database service ceiling;
- initial course 180-359 degrees: highest even-thousand flight level at or
  below the database service ceiling.

For an A339 with a 41000 ft service ceiling this gives FL410 eastbound and
FL400 westbound. Never schedule above the database ceiling. Document that true
bearing is only an approximation for the magnetic-track rule and regional ATC
exceptions.

### 4.3 Initial cruise level and steps

```text
initial_cruise_ft = usable_ceiling_ft - 2000 * steps_below_ceiling
```

Clamp to the usable ceiling and to no lower than the larger of FL280 and
`usable_ceiling_ft - 6000`.

With the statistical distance correction and A339 range 5500 NM, the intended
anchors are approximately:

- SYD-NOU, 1061 NM: climb directly to FL410 eastbound;
- NOU-BKK, 4402 NM: start around FL360 westbound, then step to FL400;
- BKK-NOU, 4395 NM: start around FL370 eastbound, then step to FL410;
- CDG-BKK, 5101 NM: start around FL350 eastbound, then step to FL410; and
- BKK-CDG, 5101 NM: start around FL340 westbound, then step to FL400.

The BKK-NOU hot-departure sample started materially lower. Temperature, payload,
ATC restrictions, and day-specific flight planning are unavailable, so a
distance-only generic model must not be tuned to that outlier.

### 4.4 Upper climb and step-climb timing

Keep the database `climb_above_fl200_min` as the time from FL200 to the database
service ceiling. Scale it linearly by altitude:

```text
time(FL200 -> level) = climb3_min
                       * (level_ft - 20000)
                       / (ceiling_ft - 20000)
```

For each 2000 ft step, use the same altitude fraction. For A339 this is about
2.38 minutes per step; FL200 to FL350 is about 17.86 minutes. The total upper
climb time to FL410 remains 25 minutes.

Calculate upper-climb distance from the scaled time and an ISA-aware speed at
the segment midpoint. Use the lesser of:

- 300 KIAS converted to TAS at the midpoint altitude; and
- `climb_mach` multiplied by local ISA speed of sound.

Do not use the current fixed `climb_mach * 593.7` TAS at every altitude.

### 4.5 Plateau placement

Build the entire vertical schedule deterministically from immutable inputs:

1. climb to the calculated initial cruise level;
2. subtract initial-climb, all step-climb, descent, and terminal-profile
   distances from `profile_total_nm`;
3. divide the non-negative level-cruise distance equally among
   `steps_below_ceiling + 1` plateaus;
4. insert one 2000 ft step climb between adjacent plateaus; and
5. descend from the final scheduled level.

The schedule is rebuilt from the same inputs or cached at flight initialization;
it must not move in response to current altitude, current groundspeed, or ETA
bias. Equal-distance plateaus are a generic deterministic proxy for weight burn.

Use the DB `cruise_kt` as cruise TAS in the first implementation. Do not silently
reuse `climb_mach` as cruise Mach. A separate cruise-Mach database field may be
considered later only with authoritative data for the whole supported fleet.

## 5. Descent and terminal profile

### 5.1 Remove double counting

The descent distance remains a three-degree proxy:

```text
scheduled_descent_nm = final_cruise_altitude_ft / 300
```

The final 60 NM approach stages must overlay the last 60 NM of this descent,
not follow a descent that already reaches field elevation. Therefore:

```text
level_cruise_nm = total - climb - steps - scheduled_descent_nm
```

There is no additional subtraction of `ETAP_APPROACH_NM`. Integrate the upper
descent schedule from TOD to 60 NM, then the terminal speed stages within the
last 60 NM. This moves nominal A339 TOD into the observed range without
inventing a database altitude.

Do not add a separate fixed terminal delay in the same change. First replay the
overlaid profile; only then evaluate a terminal vectoring allowance as an
isolated experiment.

### 5.2 Sustained actual-descent latch

Pass current altitude into `etap_update()`. Actual altitude must not continuously
reshape the theoretical profile. Use it only to latch a real early descent when
all conditions persist for 90 seconds:

- theoretical phase is a level-cruise plateau;
- actual altitude is at least 2000 ft below the scheduled plateau;
- filtered vertical rate is below -300 ft/min; and
- the aircraft is not in a scheduled step climb.

Once latched, record effective covered distance and actual altitude. Rebuild the
remaining descent once from that point to the destination. Keep the latch until
profile reset. This prevents an assigned lower cruise level or altitude noise
from moving TOD and ETA repeatedly.

## 6. Bias and timing stability

### 6.1 Correct time accounting first

Use epoch time for displayed ETA epochs and a separate monotonic fractional
timestamp for filter `dt`. Both `eta.c` and `eta_profile.c` must stop inventing
0.5 seconds for duplicate whole-second epochs. Replay must exercise the same
timing interface.

All filter time-constant tests must use elapsed monotonic time and demonstrate
that 2700 real seconds produce one 2700-second bias time constant.

### 6.2 Bias eligibility and semantics

Do not learn cruise bias during initial climb, scheduled step climbs, descent,
or approach. Reset or delay bias initialization until a stable level-cruise
plateau is established.

Because measured speed is direct closure while the theoretical profile uses
stretched path distance, compare it with a closure-equivalent prediction:

```text
predicted_closure_kt = predicted_path_groundspeed_kt / stretch_scale
```

Run all fleet replays with bias entirely disabled and with the corrected bias.
Retain bias only if it improves accuracy without increasing changes, reversals,
or error span. The target is a theoretical estimator; bias is optional.

## 7. Database policy

### 7.1 Required generator change, no database write

Read the existing `airplanes.range` column in `tools/gen_perfdb.py`, validate it
as finite and positive, and generate `max_range_nm` into `perf_ac_t`. Regenerate
`perfdb_data.c` from the Offto database opened read-only.

### 7.2 Values that must not be changed from current evidence

For A339, retain 5500 NM range, 460 kt cruise TAS, 6.5/9.0/25.0 minute climb
segments, M0.82 climb cap, and 41000 ft service ceiling during the first replay
round. Code must correct the semantics before data is tuned.

### 7.3 Potential Offto corrections

Database changes belong to the Offto project and require separate explicit
authorization. If later fleet-wide evidence requires a change:

- define `range` as operational maximum still-air route distance in NM at a
  representative commercial payload, not ferry range;
- keep `ceiling` as service ceiling and stop describing it as normal cruise;
- keep climb-above-FL200 time explicitly defined as FL200 to service ceiling;
- add a distinct nullable `cruise_mach` only when authoritative values exist for
  all affected rows; and
- validate changes across every aircraft/flight available, not only A339.

Route stretch must never be stored in an aircraft performance row. It describes
network geometry. A future directional airport-pair prior belongs in a separate
statistics table with sample count, median, dispersion, and fallback to the
generic distance curve.

## 8. Diagnostics and replay requirements

Extend replay CSV and summary diagnostics with:

- raw total and remaining GC NM;
- route-stretch percentage and effective total/remaining NM;
- range fraction;
- usable ceiling and initial cruise level;
- scheduled phase and scheduled flight level;
- step-climb boundaries;
- descent-latch status and latched point;
- bias eligibility, raw ratio, and filtered ratio; and
- raw theoretical ETA before display conditioning.

Replay altitude must be interpolated and passed to the engine rather than
discarded. Report coverage gaps separately so a gap boundary is not diagnosed as
an estimator instability.

## 9. Validation and acceptance

### 9.1 Host tests

Tests must cover:

- stretch anchors at 1061, 4395, and 5101 NM;
- stretch monotonicity, 0-5.5% bounds, and independence from remaining distance;
- identical raw and effective completion fractions;
- `max_range_nm` generation and A339 value 5500;
- direction-compatible ceiling selection;
- A339 altitude anchors listed in section 4.3;
- initial level monotonicity with range fraction and 2000 ft grid alignment;
- upper-climb scaling and approximately 2.38 minutes per A339 step;
- ISA/Mach speed-cap spot checks;
- fixed equal-distance plateau boundaries;
- no ETA discontinuity greater than the existing one-minute display creep at a
  scheduled step boundary;
- no bias learning during climb or steps;
- overlaid descent/approach distance rather than summed distances;
- 90-second early-descent hysteresis and one-shot latch;
- exact monotonic time-constant behavior at 2 Hz; and
- origin/destination or aircraft change resetting all frozen profile state.

Increase `ETAP_MAX_BP` only after calculating the maximum table size for three
steps, four cruise plateaus, wind subdivision, descent integration, and terminal
stages. A target of at least 96 is expected, but the test must prove capacity and
`push()` must not silently truncate a valid maximum-size profile.

### 9.2 Replay experiments

Run every available A339 flight in a matrix that isolates causes:

1. current production baseline;
2. corrected timing only;
3. vertical profile only;
4. route stretch only;
5. vertical profile plus route stretch, bias disabled; and
6. complete candidate with corrected bias.

Do not compare `--no-winds` with production winds to tune aircraft data. Wind
differences are explicitly outside the work. The no-bias run is required because
it isolates the theoretical profile from closure-learning side effects.

### 9.3 Acceptance criteria

- SYD-NOU receives effectively no route stretch and climbs directly near the
  usable ceiling.
- Approximately 4395 NM sectors receive about 2.3% or 100 NM, not a flat 4.2%.
- Approximately 5101 NM sectors receive about 5.5% or 281 NM.
- Near-range A339 flights start near FL340/350 according to direction and step
  to FL400/410; short A339 flights start near the ceiling.
- Scheduled steps never reverse or move after initialization.
- The raw displayed orthodromic distance is unchanged.
- Final ETA remains within approximately two minutes on complete tracks.
- Fleet stability does not regress beyond the current means of 49.9 changes,
  3.1 reversals, and 25.3 minutes of span.
- Prefer the candidate with the lowest fleet median absolute error subject to
  the stability limits; never select a database value from one flight.

## 10. Explicit non-goals

- Recovering or approximating an actual flight-plan route.
- Using daily actual winds to tune statistical winds.
- Learning a route stretch from the current flight.
- Modeling payload, temperature, cost index, ATC restrictions, or magnetic
  variation without inputs.
- Modifying the Offto database from this repository.
- Correcting unrelated UI behavior.

## 11. Review notes (Claude, 2026-07-16)

Evidence items verified against code and the 2026-07-15 replay data; the
design is agreed in substance. Corrections and additions before implementing:

1. **τ re-tune after the timing fix (§6.1).** The dt-forcing is real
   (verified: `eta.c:88`, `eta_profile.c:124-125`; 1.0+0.5 s per wall second
   at 2 Hz) — but every validated stability number (49.9/3.1/25.3 baseline
   included) was measured WITH this bug, i.e. at an effective bias τ≈1800 s
   and output τ≈40 s. Fixing timing alone changes the operating point.
   Matrix run 2 must therefore compare "true 2700" against "true 1800"
   (today's effective behavior) and re-select — do not assume 2700.
2. **The descent latch (§5.2) is fragile as a one-shot.** A mid-cruise ATC
   descent or turbulence-driven 2000+ ft step-down latches permanently and
   the profile models continuous descent for hours. Either gate the latch on
   destination proximity (`dist_to_go < ~1.5 × alt_ft/300`) or add an
   un-latch when altitude re-stabilizes level above ~FL250 with more than
   the descent distance still to go.
3. **Uniform stretch redistributes error toward the terminal phase.** The
   measured excess is front-loaded (SIDs/airway doglegs: remaining-track
   minus GC decays from ~308 NM to 0); a constant factor under-corrects
   early and adds ~+5.5% phantom distance inside the final descent. Add one
   matrix variant with a fixed, deterministic front-loaded burn-off (or a
   taper to zero inside the last ~150 NM) before committing to uniform.
4. **Expect the terminal dip to worsen with the §5.1 overlay alone.** Today
   the too-early TOD (pessimistic) partially masks the vectoring optimism
   (measured −3…−9 min at 10–30 NM). Acceptance must track last-100 NM error
   explicitly, not just full-flight span, so the follow-up vectoring
   allowance is sized from data.
5. **Add TOD-badge stability to §8/§9.3.** TOD currently churns 44–84
   changes per long-haul flight; the overlay changes its placement. Track
   TOD changes/reversals with the same rigor as the ETA.
6. **Acceptance gaps:** add per-flight max displayed step ≤1 minute
   (existing invariant), and A20N/short-haul non-regression (the criteria
   name only A339 aggregates).
7. **`range` semantics are the calibration, not a free constant (§4.1).**
   The steps heuristic only lands on the observed FL370–390 anchors because
   the DB's 5500 NM is an "operational route distance," far below physical
   A339 range (~7200 NM). If §7.3's redefinition ever changes stored values,
   the `steps_below_ceiling` formula must be recalibrated in the same change.
8. **Vertical-profile ETA impact is second-order** (cruise-altitude choice
   moves descent distance by ~10 NM and step timing by minutes; the cached
   A339s never cruised above FL400). Stage the work: timing fix + closure
   semantics + stretch + descent overlay first — they carry the measurable
   wins; step-climb scheduling can follow separately.
9. **`actype[8]` truncation (§2.2.8)** is a two-line fix (widen to 16);
   include it or state explicitly where it is deferred.

## 12. Implementation outcome (2026-07-16)

Everything in sections 3-6 is implemented and host-tested (stretch anchors,
ceiling parity, the five FL anchors, 2.38 min step scaling, descent overlay,
proximity-gated latch, true-τ timing at 1 s cadence, MAX_BP 96 capacity).
`max_range_nm` is generated from `airplanes.range` (read-only). The §9.2
isolation matrix over the 11 A339 flights (mean chg / flips / span / abserr):

| variant | chg | flips | span | abserr |
|---|---:|---:|---:|---:|
| baseline (2026-07-15 firmware) | 49 | 3.0 | 25.6 | 12.9 |
| timing fix only | 45 | 2.8 | 25.0 | 12.9 |
| + vertical/overlay, bias on | 42 | 2.5 | 22.9 | 12.6 |
| + vertical/overlay, bias off | **25** | **0.9** | **20.7** | 13.9 |
| stretch only | 54 | 2.3 | 32.2 | 15.5 |
| full (vert+stretch+bias) | 49 | 2.3 | 28.5 | 12.7 |
| front-loaded taper (review §11.3) | 51 | 2.0 | 31.4 | 13.6 |

Selection per §6.2/§9.3: **ship timing + vertical/overlay + closure
semantics with `ETAP_STRETCH_APPLY 0` and `ETAP_BIAS_APPLY 0`** (both code
paths remain compiled, host-tested, and flippable). Uniform stretch — and
the taper variant — regress the fleet exactly as review note 3 warned,
because the slow DB cruise TAS currently cancels the geometry error on the
BKK/NOU family; re-enable stretch together with per-route performance data.
The applied bias buys 1.3 min mean accuracy for 1.7× the churn — §6.2 says
that trade loses. τ re-tune (review note 1): true-2700 beat true-1800.
Final errors stay −1.9…0 min on all 11 flights; the A20N short-haul holds
7 changes / 7-min span. The descent latch engaged on 6/11 flights and the
proximity gate kept every mid-cruise descent unlatched. Per-flight, every
flight improved except 2026-05-20 (span 43→49) — the one with 101-minute
ADS-B holes (§2.1 gap caveat).

