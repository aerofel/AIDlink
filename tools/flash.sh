#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AIDlink contributors
# Compile + flash the AIDlink firmware, auto-incrementing the build version.
#
# macOS note: esptool's config scan can hit a TCC permission error when run from under ~/Desktop.
# This script compiles/uploads using an empty ESPTOOL_CFGFILE and an out-of-tree build dir to avoid it.
#
# Usage: tools/flash.sh [serial-port]
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"          # repo root
INO="$HERE/firmware/aidlink/aidlink.ino"
PORT="${1:-/dev/cu.usbserial-0001}"
BUILD="${TMPDIR:-/tmp}/aidlink_build"
EMPTYCFG="${TMPDIR:-/tmp}/aidlink_empty.cfg"; : > "$EMPTYCFG"
# huge_app: single 3MB app partition (no OTA, no filesystem) — the firmware uses neither.
# Triples app space vs the default scheme; nvs stays at 0x9000 so saved config is preserved.
FQBN="esp32:esp32:esp32:PartitionScheme=huge_app"

# bump vNN in the FW_BUILD define (date/time come from __DATE__/__TIME__ = flash time)
cur=$(grep -oE '#define FW_BUILD \("v[0-9]+ ' "$INO" | grep -oE '[0-9]+')
next=$((cur+1)); sed -i '' "s/#define FW_BUILD (\"v$cur /#define FW_BUILD (\"v$next /" "$INO"

# stop our serial logger if it holds the port (scoped to this project only)
pkill -f "tee_serial.py" 2>/dev/null || true; sleep 0.5   # pkill exits 1 when none running; don't trip set -e

arduino-cli compile --fqbn "$FQBN" "$HERE/firmware/aidlink" --output-dir "$BUILD"
ESPTOOL_CFGFILE="$EMPTYCFG" arduino-cli upload -p "$PORT" --fqbn "$FQBN" --input-dir "$BUILD"
echo ">>> flashed v$next  — tap EN, power-cycle, or run tools/reboot.py to start it"
