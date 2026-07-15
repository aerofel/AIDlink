#!/usr/bin/env python3
"""Extract flight tracks from the onboard-ip-mock AeroAPI JSON cache.

For each cached flight, writes <out>/<name>.track.csv with rows
  t_epoch_s,lat,lon,alt_ft
and prints one meta line per flight (tab-separated):
  name flight orig dest actype off_epoch on_epoch npoints
"""
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

CACHE = Path.home() / ".cache/onboard-ip-mock"


def epoch(ts: str) -> int:
    return int(datetime.strptime(ts, "%Y-%m-%dT%H:%M:%SZ")
               .replace(tzinfo=timezone.utc).timestamp())


def main(outdir: str) -> None:
    out = Path(outdir)
    out.mkdir(parents=True, exist_ok=True)
    for kml in sorted(CACHE.glob("*.kml")):
        name = kml.stem
        jpath = kml.with_suffix(".json")
        if not jpath.exists():
            print(f"skip {name}: no JSON companion", file=sys.stderr)
            continue
        d = json.load(open(jpath))
        f = d["flight"]
        positions = d["track"]["positions"]
        rows = []
        last_t = None
        for p in positions:
            if p.get("latitude") is None or p.get("longitude") is None:
                continue
            t = epoch(p["timestamp"])
            if last_t is not None and t <= last_t:
                continue                     # keep strictly monotonic
            last_t = t
            alt_ft = float(p.get("altitude") or 0) * 100.0
            rows.append((t, p["latitude"], p["longitude"], alt_ft))
        csv = out / f"{name}.track.csv"
        with open(csv, "w") as fh:
            for r in rows:
                fh.write(f"{r[0]},{r[1]:.6f},{r[2]:.6f},{r[3]:.0f}\n")
        print("\t".join(str(x) for x in (
            name,
            f.get("ident", "?"),
            f["origin"]["code_icao"],
            f["destination"]["code_icao"],
            f.get("aircraft_type") or "",
            epoch(f["actual_off"]) if f.get("actual_off") else 0,
            epoch(f["actual_on"]) if f.get("actual_on") else 0,
            len(rows),
        )))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
