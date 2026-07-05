#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AIDlink contributors
#
# Build + flash the ESP-IDF AIDlink firmware for a chosen target.
#
#   ./flash-idf.sh esp32          # classic board, via CP2102
#   ./flash-idf.sh esp32s3        # S3, via CH343 UART port (NOT native USB)
#   ./flash-idf.sh esp32s3 /dev/cu.usbmodemXXXX [monitor]
#
# IMPORTANT (esp32s3): flash + monitor over the board's *CH343 UART* port. The
# S3's native-USB port boot-loops the 2nd-stage bootloader on this hardware, and
# in the running firmware that port is TinyUSB-NCM (a network device), not a
# console. See LEARNING.md.
set -e
TGT="${1:?usage: flash-idf.sh <esp32|esp32s3> [port] [monitor]}"

# Sensible port defaults per target (override with arg 2).
case "$TGT" in
  esp32)   DEF_PORT=/dev/cu.usbserial-0001 ;;                  # CP2102
  esp32s3) DEF_PORT=$(ls /dev/cu.usbmodem5AE* 2>/dev/null | head -1) ;;  # CH343 UART
  *) echo "unknown target: $TGT (expected esp32 or esp32s3)"; exit 1 ;;
esac
PORT="${2:-$DEF_PORT}"
if [ -z "$PORT" ]; then echo "no port found for $TGT; pass one explicitly"; exit 1; fi

: "${IDF_PATH:=$HOME/esp/esp-idf}"
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" >/dev/null 2>&1

cd "$(dirname "$0")"
# set-target regenerates sdkconfig from sdkconfig.defaults[.<target>]; safe to repeat.
idf.py set-target "$TGT"
if [ "${3:-}" = "monitor" ]; then
  idf.py -p "$PORT" flash monitor
else
  idf.py -p "$PORT" flash
  echo ">>> flashed $TGT on $PORT"
fi
