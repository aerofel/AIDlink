#!/usr/bin/env python3
"""Replay a cached real flight through the firmware ETA engine and compare
the displayed ETA against the actual track and the actual arrival time.

The calculation engine IS the firmware source (host_test/replay_eta.c links
main/eta.c, eta_profile.c, derive.c, geo.c, perfdb.c, airports.c and mirrors
the poller 1 Hz + display 500 ms wiring); this script only feeds it a real
flown track and scores the output. Stdlib only; auto-builds the C harness
with clang on first run.

Examples:
    tools/replay_flight.py --list
    tools/replay_flight.py 2026-07-12_ACI501_VTBS
    tools/replay_flight.py 2026-07-12_ACI501_VTBS --step 30
    tools/replay_flight.py ~/.cache/onboard-ip-mock/2026-07-12_ACI501_VTBS.kml
    tools/replay_flight.py 2026-07-12_ACI501_VTBS --cruise 498 --no-bias

Each table row: time into flight, UTC, dist-to-go, derived GS, the ETA the
screen would show, the TRUE time remaining at that instant, and the error
(shown ETA - actual touchdown). Ground truth is AeroAPI actual_on when the
JSON cache exists; with a bare KML the last track point stands in (the track
usually cuts a few minutes before touchdown - noted in the output).
"""
import argparse
import csv
import math
import json
import re
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW = ROOT / "firmware-idf"
CACHE = Path.home() / ".cache/onboard-ip-mock"
ENGINE_SRCS = ["host_test/replay_eta.c", "main/eta.c", "main/eta_profile.c",
               "main/derive.c", "main/geo.c", "main/perfdb.c",
               "main/perfdb_data.c", "main/airports.c"]


def epoch(ts):
    return int(datetime.strptime(ts, "%Y-%m-%dT%H:%M:%SZ")
               .replace(tzinfo=timezone.utc).timestamp())


def hhmm(ep):
    return datetime.fromtimestamp(ep, tz=timezone.utc).strftime("%H:%M")


def build_engine():
    out = FW / "build-host" / "replay_eta"
    srcs = [FW / s for s in ENGINE_SRCS]
    if out.exists() and all(out.stat().st_mtime > s.stat().st_mtime for s in srcs):
        return out
    out.parent.mkdir(exist_ok=True)
    print("building ETA engine harness...", file=sys.stderr)
    subprocess.run(["clang", "-I" + str(FW / "main"), "-O2", "-o", str(out)]
                   + [str(s) for s in srcs] + ["-lm"], check=True)
    return out


def load_json(jpath):
    d = json.load(open(jpath))
    f = d["flight"]
    pts, last = [], None
    for p in d["track"]["positions"]:
        if p.get("latitude") is None:
            continue
        t = epoch(p["timestamp"])
        if last is not None and t <= last:
            continue
        last = t
        pts.append((t, p["latitude"], p["longitude"],
                    (p.get("altitude") or 0) * 100.0, p.get("groundspeed") or 0))
    return {
        "flight": f.get("ident", "?"),
        "orig": f["origin"]["code_icao"], "dest": f["destination"]["code_icao"],
        "actype": f.get("aircraft_type") or "",
        "off": epoch(f["actual_off"]) if f.get("actual_off") else pts[0][0],
        "on": epoch(f["actual_on"]) if f.get("actual_on") else None,
    }, pts


def load_kml(kpath):
    """gx:Track fallback: relative timestamps, no true touchdown time."""
    s = open(kpath).read()

    def xdata(name):
        m = re.search(rf'Data name="{name}"><value>([^<]+)</value>', s)
        return m.group(1) if m else ""
    whens = [epoch(w[:19] + "Z") for w in re.findall(r"<when>([^<]+)</when>", s)]
    coords = [tuple(float(x) for x in c.split())
              for c in re.findall(r"<gx:coord>([^<]+)</gx:coord>", s)]
    # cache KMLs carry offsets from 2000-01-01; rebase onto the flight date
    # from the filename so the wind-season (month) is right
    m = re.match(r"(\d{4}-\d{2}-\d{2})_", Path(kpath).name)
    if m and whens:
        base = epoch(m.group(1) + "T12:00:00Z") - whens[0]
        whens = [t + base for t in whens]
    pts = [(t, la, lo, alt * 3.28084, 0)
           for t, (lo, la, alt) in zip(whens, coords)]
    return {
        "flight": xdata("callsign"), "orig": xdata("origin"),
        "dest": xdata("destination"), "actype": "A339",
        "off": pts[0][0], "on": None,
    }, pts


def resolve(arg):
    p = Path(arg).expanduser()
    if p.suffix == ".json" and p.exists():
        return load_json(p)
    if p.suffix == ".kml" and p.exists():
        j = p.with_suffix(".json")
        return load_json(j) if j.exists() else load_kml(p)
    j = CACHE / f"{arg}.json"
    if j.exists():
        return load_json(j)
    k = CACHE / f"{arg}.kml"
    if k.exists():
        return load_kml(k)
    sys.exit(f"flight '{arg}' not found (looked for {j} and {k})")


def list_flights():
    for j in sorted(CACHE.glob("*.json")):
        if j.name.count(".") > 1:
            continue
        try:
            meta, pts = load_json(j)
        except (KeyError, TypeError):
            continue
        dur = (pts[-1][0] - pts[0][0]) / 3600
        on = hhmm(meta["on"]) + "z" if meta["on"] else "?"
        print(f"{j.stem:<26} {meta['flight']:<7} {meta['orig']}>{meta['dest']}"
              f"  {meta['actype']:<5} {dur:4.1f} h  touchdown {on}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("flight", nargs="?", help="cache name, .json or .kml path")
    ap.add_argument("--list", action="store_true", help="list cached flights")
    ap.add_argument("--step", type=int, default=15, help="table step, minutes")
    ap.add_argument("--actype", help="override perfdb type (default: from feed)")
    ap.add_argument("--cruise", help="override cruise TAS kt")
    ap.add_argument("--no-winds", action="store_true")
    ap.add_argument("--no-bias", action="store_true")
    ap.add_argument("--csv", help="also dump the full 500 ms series here")
    args = ap.parse_args()

    if args.list or not args.flight:
        list_flights()
        return

    meta, pts = resolve(args.flight)
    actype = args.actype or meta["actype"]
    on = meta["on"]
    approx = on is None
    if approx:
        on = pts[-1][0]

    engine = build_engine()
    with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as tf:
        for t, la, lo, al, gs in pts:
            tf.write(f"{t},{la:.6f},{lo:.6f},{al:.0f},{gs:.0f}\n")
        track = tf.name
    cmd = [str(engine), track, meta["orig"], meta["dest"], actype]
    if args.no_winds:
        cmd.append("--no-winds")
    if args.no_bias:
        cmd.append("--no-bias")
    if args.cruise:
        cmd += ["--cruise", args.cruise]
    r = subprocess.run(cmd, capture_output=True, text=True, check=True)
    if r.stderr:
        print(r.stderr.strip(), file=sys.stderr)
    rows = list(csv.DictReader(r.stdout.splitlines()))
    if args.csv:
        Path(args.csv).write_text(r.stdout)

    print(f"\n{meta['flight']}  {meta['orig']} > {meta['dest']}  ({actype})"
          f"  off {hhmm(meta['off'])}z"
          f"  touchdown {hhmm(on)}z{' (track end - approx)' if approx else ''}\n")
    print(f"{'T+':>6} {'utc':>6} {'to-go':>7} {'GS':>5} {'shown ETA':>10}"
          f" {'true rem':>9} {'error':>7}")

    t0 = int(rows[0]["t_s"])
    nxt = t0
    ch, flips, lastdir, prev = 0, 0, 0, None
    emin, emax, final = 1e9, -1e9, None
    first_shown = None
    for row in rows:
        t, m = int(row["t_s"]), int(row["disp_min"])
        if m > 0:
            if first_shown is None:
                first_shown = t
            e = m - on / 60.0
            emin, emax, final = min(emin, e), max(emax, e), e
            if prev is not None and m != prev:
                d = m - prev
                if d * lastdir < 0:
                    flips += 1
                lastdir = d
                ch += 1
            prev = m
        if t >= nxt:
            nxt += args.step * 60
            el = t - t0
            eta_s = m * 60
            print(f"{el//3600:3d}:{el%3600//60:02d} {hhmm(t):>6}"
                  f" {float(row['dist_nm']):>6.0f}N {float(row['gs_kt']):>5.0f}"
                  f" {hhmm(eta_s) + 'z' if m > 0 else '--:--':>10}"
                  f" {(on - t)/60:>8.0f}m"
                  f" {m - on/60.0:>+6.1f}m" if m > 0 else
                  f"{el//3600:3d}:{el%3600//60:02d} {hhmm(t):>6}"
                  f" {float(row['dist_nm']):>6.0f}N {float(row['gs_kt']):>5.0f}"
                  f" {'--:--':>10} {(on - t)/60:>8.0f}m {'':>7}")

    print(f"\nsummary: first ETA {int((first_shown - t0))} s after first fix"
          f" | {ch} displayed changes, {flips} direction flips"
          f" | error span [{emin:+.1f} .. {emax:+.1f}] min"
          f" | final {final:+.1f} min vs touchdown")
    if approx:
        print("note: no AeroAPI JSON - 'touchdown' is the last KML point,"
              " typically a few minutes before the real landing")


if __name__ == "__main__":
    main()
