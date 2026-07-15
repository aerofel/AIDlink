#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AIDlink contributors
#
# Generate firmware-idf/main/perfdb_data.c from the Offto project's SQLite
# database (READ-ONLY — the DB is owned by the Offto iOS app and is never
# modified or copied; this repo commits only the generated C table).
#
#   airplanes         -> PERFDB_AC[]  (make/model/type, cruise TAS, climb
#                        segment minutes, climb Mach, ceiling)
#   wind_climatology  -> PERFDB_U/V250 + U/V300 int8 m/s seasonal grids
#                        (ERA5 5-yr means, 5° lat bands x 60° lon sectors)
#
# Re-run manually whenever the Offto DB changes:
#   python3 tools/gen_perfdb.py
import argparse
import datetime
import os
import sqlite3
import sys

DEF_DB = os.path.expanduser("~/Sites/Swift/Offto/Resources/offto.sqlite")
DEF_OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "firmware-idf", "main", "perfdb_data.c")

SEASONS = ["DJF", "MAM", "JJA", "SON"]
LATS = list(range(-90, 95, 5))          # 37 band floors
LONS = list(range(-180, 180, 60))       # 6 sector floors


def cf(x):
    """float literal with a guaranteed decimal point (7 -> '7.0f')"""
    s = f"{float(x):g}"
    if "." not in s and "e" not in s:
        s += ".0"
    return s + "f"


def wind_grid(cur, hpa):
    u = [[[0] * len(LONS) for _ in LATS] for _ in SEASONS]
    v = [[[0] * len(LONS) for _ in LATS] for _ in SEASONS]
    n = 0
    for season, lat_min, lon_min, um, vm in cur.execute(
            "SELECT season, lat_min, lon_min, u_mean_ms, v_mean_ms "
            "FROM wind_climatology WHERE pressure_hpa=?", (hpa,)):
        si = SEASONS.index(season)
        li = LATS.index(lat_min)
        gi = LONS.index(lon_min)
        iu, iv = round(um), round(vm)
        if not (-127 <= iu <= 127 and -127 <= iv <= 127):
            sys.exit(f"wind mean out of int8 range at {season}/{lat_min}/{lon_min}: {um},{vm}")
        u[si][li][gi] = iu
        v[si][li][gi] = iv
        n += 1
    if n != len(SEASONS) * len(LATS) * len(LONS):
        sys.exit(f"wind_climatology@{hpa}hPa: expected {len(SEASONS)*len(LATS)*len(LONS)} boxes, got {n}")
    return u, v


def emit_grid(f, name, g):
    f.write(f"const int8_t {name}[4][37][6] = {{\n")
    for si, season in enumerate(SEASONS):
        f.write(f"    {{ // {season}\n")
        for li, lat in enumerate(LATS):
            row = ", ".join(f"{x:4d}" for x in g[si][li])
            f.write(f"        {{ {row} }},{' // lat ' + str(lat) if li % 6 == 0 else ''}\n")
        f.write("    },\n")
    f.write("};\n\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=DEF_DB)
    ap.add_argument("--out", default=os.path.normpath(DEF_OUT))
    args = ap.parse_args()

    con = sqlite3.connect(f"file:{args.db}?mode=ro", uri=True)
    cur = con.cursor()

    ac = list(cur.execute(
        "SELECT make, model, type, speed, climb_to_fl100_min, "
        "climb_fl100_to_fl200_min, climb_above_fl200_min, climb_mach, ceiling "
        "FROM airplanes ORDER BY make, model"))
    u250, v250 = wind_grid(cur, 250)
    u300, v300 = wind_grid(cur, 300)
    con.close()

    with open(args.out, "w") as f:
        f.write("// SPDX-License-Identifier: Apache-2.0\n"
                "// Copyright 2026 AIDlink contributors\n"
                "//\n"
                "// GENERATED FILE — do not edit. Regenerate with tools/gen_perfdb.py\n"
                f"// Source: {args.db} (read-only)\n"
                f"// Generated: {datetime.date.today().isoformat()} — "
                f"{len(ac)} aircraft, {len(SEASONS)}x{len(LATS)}x{len(LONS)} wind boxes @250+300 hPa\n"
                "#include \"perfdb.h\"\n\n")
        f.write(f"const int PERFDB_NAC = {len(ac)};\n")
        f.write("const perf_ac_t PERFDB_AC[] = {\n")
        for make, model, typ, spd, c1, c2, c3, mach, ceil in ac:
            f.write(f'    {{ "{make}", "{model}", "{typ}", {spd}, '
                    f"{cf(c1)}, {cf(c2)}, {cf(c3)}, {cf(mach)}, {int(ceil)} }},\n")
        f.write("};\n\n")
        emit_grid(f, "PERFDB_U250", u250)
        emit_grid(f, "PERFDB_V250", v250)
        f.write("// 300 hPa (~FL300) — reserved for altitude interpolation, unused in v1.\n")
        emit_grid(f, "PERFDB_U300", u300)
        emit_grid(f, "PERFDB_V300", v300)
    print(f"wrote {args.out}: {len(ac)} aircraft + 4 wind grids")


if __name__ == "__main__":
    main()
