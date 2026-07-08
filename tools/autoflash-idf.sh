#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AIDlink contributors
#
# Flash the current firmware-idf build the moment an ESP32 downloader/serial
# port appears, retrying through transient enumeration windows. Pairs with the
# web portal's "Firmware update…" button (/dfu) on single-USB-port boards
# (T-Display-S3): click the button, run this, tap RST when it reports FLASH OK.
#
#   tools/autoflash-idf.sh                 # wait for any cu.usbmodem*
#   tools/autoflash-idf.sh esp32s3         # explicit chip (default esp32s3)
#
# Notes (see LEARNING.md 2026-07-06):
#  - after flashing over native USB the S3 usually re-latches into download
#    mode on the soft reset — finish with one physical RST tap.
#  - a USB replug is NOT a power cycle when a battery is connected.
set -u
CHIP="${1:-esp32s3}"
cd "$(dirname "$0")/../firmware-idf/build" || { echo "no build dir — run idf.py build first"; exit 1; }
PY="${IDF_PYTHON_ENV_PATH:-$HOME/.espressif/python_env/idf5.3_py3.14_env}/bin/python"
[ -x "$PY" ] || PY=python3

echo ">>> waiting for a serial port (click 'Firmware update…' in the portal, or hold BOOT+tap RST)"
for i in $(seq 1 3600); do
  PORT=$(find /dev -maxdepth 1 -name 'cu.usbmodem*' 2>/dev/null | head -1)
  if [ -n "$PORT" ]; then
    echo ">>> $PORT seen — flashing ($CHIP)"
    if "$PY" -m esptool --chip "$CHIP" -b 460800 --before default_reset --after hard_reset \
         -p "$PORT" write_flash @flash_args; then
      echo ">>> FLASH OK — if the device stays in the bootloader, tap RST once"
      exit 0
    fi
    echo ">>> attempt failed (transient port window?) — retrying"
  fi
  sleep 0.25
done
echo ">>> timed out waiting for a port"
exit 1
