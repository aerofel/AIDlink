# The ETA estimator — architecture, real-flight accuracy, and tooling

*2026-07-15. Companion to the deep-dive analysis in
[superpowers/specs/2026-07-15-eta-stability-replay.md](superpowers/specs/2026-07-15-eta-stability-replay.md).*

## How the displayed ETA is produced

Two estimators run side by side in the display task (`display.c`):

1. **Made-good fallback** (`eta.c`) — distance-to-go over a 10-min windowed
   ground speed, blended with the instantaneous derived speed while the
   window fills. Always runs; owns the sample ring. Shown only when no
   aircraft profile is resolved.
2. **Theoretical profile** (`eta_profile.c`) — an FMS-style prediction of the
   whole remaining flight: DB climb segments, cruise TAS with seasonal
   250 hPa climatology winds (5°×60° boxes, wind triangle per segment),
   integrated descent schedule, staged 60 NM approach. One adaptive term: a
   cruise-bias EMA (measured made-good vs predicted GS, τ 45 min, clamp
   ±10 %, weighted by fraction of cruise flown). Overrides the fallback and
   adds the TOD readout whenever `perfdb_find(CFG->perf_type)` resolves and
   the route is known.

Aircraft data comes from the Offto app's SQLite via `tools/gen_perfdb.py`
(committed as `perfdb_data.c`, 31 aircraft). The feed's `aircraftType`
pre-selects the profile (poller.c); neo codes without exact rows alias to
their ceo family row (`perfdb.c`).

## What real flights measure (12 cached Aircalin flights, 2026-07)

Replaying real AeroAPI tracks through the exact firmware pipeline
(`tools/replay_flight.py`) against actual touchdown times:

- Final accuracy was always good: **−2…+1 min** at track end.
- Mid-flight the displayed ETA wandered a **20–44 min envelope** with
  ~54 changes/flight, every change a 2-min jump — not FMS-steady.

Error budget (A339 fleet), from reciprocal-pair TAS measurement, climatology
vs actual wind comparison, and oracle-winds counterfactual replays:

| cause | size | fixable? |
|---|---|---|
| cruise TAS: DB 460 kt vs real ~498 (BKK/NOU legs) / ~465 (CDG legs) | up to ~40 min early-flight | partially — route-dependent |
| climatology wind vs actual | ±10–22 kt, sign flips day-to-day → ±3–30 min | no (offline); live winds would |
| route vs great-circle (CDG legs +300 NM, front-loaded) | 35–45 min, dominates CDG legs | needs a per-route prior |
| bias EMA τ 600 s chasing gusts | ×3 displayed churn | **fixed** |
| hysteresis 90 s > 60 s ⇒ 2-min display jumps | every change | **fixed** |
| A20N missing from perfdb → reactive fallback shown | 173 changes, 295-min span on a 2.4 h leg | **fixed** |

Beware two measured cancellations before tuning anything: the slow DB TAS
masked real headwinds on some days, and on CDG legs the climatology error
partially hides the great-circle optimism. Judge any change by replaying the
whole cached flight set, never one flight.

## Corrections applied (2026-07-15)

1. **A20N row** added to the Offto DB (Aircalin A320neo) with performance
   deduced from the 2026-07-14 ACI141 track: cruise 445 kt (measured GS 486
   − climatology tailwind, corrected for the route's measured climatology
   bias), climb 7.5 / 4.8 / 13.0 min, M0.78, ceiling 39 000. Single-flight
   deduction — refine when more A20N tracks are cached. `perfdb_data.c`
   regenerated; `perfdb_find()` additionally aliases A19N→A319, A21N→A320.
2. **Bias EMA τ 600 → 2700 s** (`ETAP_BIAS_TAU_S`): tracks the route-average
   deviation instead of local gusts.
3. **Display conditioning** (`condition()` in `eta_profile.c`): hysteresis
   grows +60 s per hour-to-go beyond 1 h (cap 420 s), and the shown minute
   creeps ±1 toward the smoothed epoch instead of re-rounding.

Replay of the 11 A339 flights, before → after:
**changes 54×2-min → 49×1-min, direction flips 10 → 3, error span
31.8 → 25.6 min**, final accuracy unchanged. The A20N flight:
**173 changes/295-min span → 12 changes/6-min span.**
Host tests cover the new behavior (`test_perfdb.c`, `test_eta_profile.c`:
step granularity ≤1 min, far-out hold, +40 min bias sampling).

Tested and **rejected** (don't retry without new evidence): replacing the
bias p-scaling with an observation-time confidence ramp (span ×2–3), and
in-flight cumulative route-stretch (front-loaded excess ⇒ overestimates).

## Residual discrepancy taxonomy (post-fix replay, 2026-07-15)

Re-running `replay_flight.py` over all 12 flights with the corrections in:

1. **Route-family bias, BKK↔NOU eastbound** — flat +23…+30 min for the
   first 5–6 h (display now rock-steady: 0–2 flips), burning off in the last
   3 h as the real tailwind (GS 530–570 kt) exceeds prediction. DB TAS +
   climatology under-calling the eastbound jet; per-route data would fix it.
2. **Day-wind wander, CDG legs** — ±10…20 min with several zero crossings
   (e.g. VTBS→LFPG 07-13: −15 → +10 → 0). The day's jet vs seasonal means;
   not correctable offline — this is what live winds would remove.
3. **Geometry optimism, CDG legs, early** — the −14…−20 min at departure is
   partly the +300 NM route-vs-GC excess; decays as flown; entangled with 2
   (they partially cancel on some days).
4. **Gap-boundary artifacts (replay-side only)** — sharp 10–17 min swings
   inside one step, aligned with 45–100 min ADS-B holes (Iran corridor,
   oceanic): e.g. LFPG→VTBS 07-13, −17.4 → −0.4 right at the T+6.4 h gap.
   The real Viasat feed is gapless; these inflate CDG-leg change counts.
5. **Terminal-area dip** — −3…−9 min at 10–30 NM to go, recovering to
   −2…0 by touchdown, on every flight: the staged 60 NM approach model is
   faster than real vectoring/sequencing. Bounded and self-healing.

Non-issues after the fixes: ETA appears in 1–2 s everywhere; all displayed
changes are ±1-min creeps; the A20N flight holds within ±5 min end to end.

## Remaining limits & future candidates

- The ±25 min mid-flight envelope that remains is wind + geometry, not
  algorithm: live winds aloft when `netcore_inet_up()` (fetch a few points
  along the remaining great circle, fall back to climatology) and a
  per-route learned stretch/wind residual in NVS are the two candidates.
- TOD badge: model puts TOD at `ceiling/300 + 60` NM to go (~197 NM for
  A339); real flights descend at 99–162 NM. TOD churns like the ETA did and
  only partially benefits from the conditioning (nearer horizon).
- Replay fidelity: cached tracks carry ADS-B coverage gaps bridged linearly;
  the real Viasat feed is gapless. A/B comparisons are solid; absolute
  numbers approximate.

## Tooling

```bash
tools/replay_flight.py --list                 # cached flights
tools/replay_flight.py 2026-07-12_ACI501_VTBS # step table + summary
tools/replay_flight.py <name> --step 30 --cruise 498 --no-bias --csv out.csv
python3 tools/gen_perfdb.py                   # after any Offto DB change
```

`replay_flight.py` auto-builds `host_test/replay_eta.c` (which links the
actual firmware modules) into `firmware-idf/build-host/` and compares the
shown ETA at each step against the actual track and touchdown.
`tools/extract_track.py` dumps raw track CSVs from the AeroAPI cache
(`~/.cache/onboard-ip-mock/`, populated by starLOG's onboard-ip-mock tools).

## Conclusion (2026-07-15)

**The display problem is solved; the remaining error is a data problem.**
After the corrections, the shown ETA behaves like an FMS readout — appears
within seconds, creeps by single minutes, almost never reverses (flips
10→3 fleet-mean, 0–2 on Pacific legs), and lands within ±2 min — but it can
still sit 10–30 min off mid-flight because the model's inputs (one cruise
TAS, seasonal winds, great-circle distance) don't know the route or the day.

Priority order for future work, by measured value:

1. **Per-route cruise data** — biggest fixable chunk (+23…30 min flat bias
   on BKK↔NOU eastbound; measured TAS ~498 kt vs DB 460). Either better DB
   values per route family or an NVS-learned per-city-pair residual.
2. **Terminal-stage tuning** — one-constant experiment (slow the last-25 NM
   approach stages or add a vectoring allowance) to flatten the systematic
   −3…−9 min dip at 10–30 NM out.
3. **Per-route stretch prior** — the CDG legs' +300 NM route-vs-GC excess;
   must come from stored priors, NOT in-flight measurement (tested, rejected).
4. **Live winds aloft when online** — the only fix for the ±10…20 min
   day-wind wander; would collapse the mid-flight envelope toward ±10 min.

Guardrails learned the hard way — read before touching the estimator:

- **Errors cancel pairwise** (slow TAS ↔ missed headwinds; wind error ↔
  geometry optimism). Never judge a change on one flight; always
  `replay_flight.py` the whole cached set and compare means.
- **The bias p-scaling is leverage control.** Making the bias more confident
  or faster re-creates the flapping (confidence-ramp: span ×2–3; τ 600:
  churn ×3). Rejected twice; don't retry without new evidence.
- **Replay numbers on CDG legs are pessimistic** — ADS-B gaps (45–100 min,
  Iran corridor) create artificial 10–17 min steps the real Viasat feed
  won't produce.
- The A20N perfdb row is a **single-flight deduction** (2026-07-14 ACI141);
  re-derive when more A20N tracks land in the cache.
