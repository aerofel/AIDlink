# ADBP dead-reckoning hold — keep the Jepp FD ownship alive through feed stalls

_Spec, 2026-08-13. Status: **implemented same day** (DR hold + OnEvent fallback
+ subscribe/transition logging + `pos_state` on /status + persistent flash log
`flog` on a new 512 KB partition with per-minute poll summaries). Host-tested
(`test_adbp_hold.c`, `test_flog_core.c`) and flashed to Board 3. Follows the
in-flight observation that the FliteDeck ownship symbol disappears and
re-appears when the cabin-Wi-Fi feed stalls._

## The observed failure, explained

When HTTPS polls of the onboard feed fail (weak/far cabin AP: RTT spikes
>1000 ms, ping gaps), `s_feed.at_ms` stops advancing. Nothing else happens
until the age crosses `stale_ms`, then two things kill the ownship:

1. **Periodic subscriptions** keep receiving frames, but every positional and
   identity parameter collapses to `<parameter name="…" validity="2"/>` (NCD,
   no value) and `GNSS_AVAIL` flips to `0` (`adbp.c:53-58`,
   `adbp_frame.c:79,128-129`). Coarse+fine lat/lon go NCD together, so the EFB
   has no fix at all and drops the symbol (README.md:209-219).
2. **OnEvent subscriptions** stop receiving *anything* at the **first** failed
   fetch — `due` requires `p.last_fix_ms` to change (`adbp.c:176,182`) and it
   never does. The EFB then ages the position out on its own clock.

The unit in service (Board 3, T-Display-S3) still carries the legacy NVS
values: **`stale_ms=15000`, `poll_ms=4000`** (read live 2026-08-13; code
defaults are 30000/1000 but NVS wins). Four missed polls ≈ two 8 s HTTP
timeouts → NCD cliff. On a lossy link that clusters failures (each error
closes the keep-alive socket, so the retry pays a fresh TLS handshake), this
fires "relatively often" — matching the report.

Within the fresh window the firmware already **dead-reckons**: `build_params`
advances a local copy along last track/GS by `age` seconds, `dt` clamped to
`stale_ms/1000` (`adbp.c:55-58`, `adbp_frame.c:31-41`). The failure is not
that we freeze — it is the hard cliff from "DR-extrapolated, validity=1" to
"nothing".

## Options considered

| Option | Verdict |
|---|---|
| A. Send nothing while stuck | ❌ This is what OnEvent subs get today and the symbol still disappears — the EFB ages out on its own. Silence also makes "feed stuck" indistinguishable from "AID dead". |
| B. Freeze last position, validity=1 | ❌ Strictly worse than DR, which already exists: frozen error grows ~8 NM/min at cruise GS; DR error grows only with wind/track change (≈1-2 NM over 5 min in cruise). |
| C. **Extend the DR window, then NCD (bounded)** | ✅ Recommended. Position stays, moves plausibly, error bounded by a cap, honest NCD after the cap. |
| D. GNSS as fallback source | ✅ Already shipped (arbiter + wired GNSS, bench-validated 2026-07-25/26); gives a *real* position instead of a modeled one, but needs the receiver wired + powered + sky view. Complementary to C, not a substitute — C protects every board. |

## Recommended design (option C + OnEvent fix, GNSS-compatible)

### 1. Split the two meanings of `stale_ms`

Today `stale_ms` conflates "how long a fix is trusted" (arbiter, `poller.c:265`)
with "when ADBP stops reporting" (`adbp.c:54`). Keep them separate:

- `stale_ms` (default 30000): unchanged meaning — fix freshness for the
  arbiter. Keeping it short preserves prompt automatic fallback to GNSS when
  a receiver is present (`possrc_choose`, 5 s GNSS window).
- **New `dr_hold_ms`** (NVS + portal field, default **300000**, clamp
  0–900000; 0 = today's behavior): for `stale_ms ≤ age < stale_ms+dr_hold_ms`
  ADBP keeps emitting `validity="1"` frames, dead-reckoned with the *full*
  `age` (raise the `dt` clamp), `GNSS_AVAIL=1`.

### 2. Degrade the quality figures during the hold

While in DR hold, don't pretend full accuracy: grow `FOM` from 8.0 and
`HDOP/VDOP` from 0.8 with age (e.g. FOM `8+age_min*4`, HDOP
`0.8+age_min*0.5`). If FliteDeck consumes them it can shade the symbol; if it
ignores them, harmless.

### 3. Fix the OnEvent silence

While position is fresh-or-DR but `last_fix_ms` is not advancing, OnEvent subs
must still be served: push at a fallback cadence (reuse `su->period_ms`,
i.e. 5000 default) so the EFB keeps receiving the DR position. One-line
concept: `due = (fix changed) || (valid_or_dr && now-last_push ≥ period)`.
This may well be the dominant fix — we do not yet know which mode FliteDeck
negotiates. **Add a `/log` line on subscribe recording mode + refreshperiod**
so the next flight answers that.

### 4. After the hold: honest NCD

Past `stale_ms + dr_hold_ms`, exactly today's NCD frames. A bounded lie is a
usability feature; an unbounded one is a navigation hazard. 5 min of DR at
cruise ≈ 1-3 NM typical error; in a turn it degrades fast, which is why the
cap exists and why quality figures degrade.

### 5. Display/status coherence

Display and `/status` gate "valid" on the same `stale_ms` (`web.c:776`).
Show DR state distinctly there (e.g. amber "DR" tag / `"live_source":"dr"`),
so a stuck feed is visible on the device even while the EFB keeps its symbol.

## Zero-code mitigation available today

On the portal, raise **Stale → NCD (ms)** from 15000 to e.g. **120000–300000**
(no upper clamp; min 1000). Because DR already covers the whole fresh window,
this alone keeps the symbol alive and moving through multi-minute stalls for
**periodic** subs (not OnEvent), at the cost of also delaying GNSS fallback
and marking the display/status valid while stale. Optionally lower
`poll_ms` 4000→1000-2000 to detect recovery sooner. `/save` reboots the unit —
do it on the ground.

## Verification plan

- Host test: frame content at age < stale, stale..stale+hold (validity=1,
  DR advances, FOM/HDOP grow), > hold (NCD); OnEvent sub receives pushes at
  period while fix seq is frozen; `dr_hold_ms=0` reproduces today byte-for-byte.
- Bench: subscribe a fake EFB (nc), kill the uplink, watch frames across both
  boundaries.
- Flight: `/log` shows subscribe mode; symbol persists through a stall.

## Out of scope (noted for later)

- `connect_push()` blocking `connect()` with no timeout can stall the whole
  ADBP task if an EFB vanishes (`adbp.c:169`) — pre-existing, separate fix.
- The ping spikes/gaps to the cabin AP are RF, not firmware; only better
  antenna placement or AP proximity helps. They are the *trigger*, and DR
  hold is the correct absorber.
