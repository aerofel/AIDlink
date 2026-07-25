#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AIDlink contributors
#
# Flash a running AIDlink unit over its USB-C cable with NO physical button
# presses. Works on Board 3 / 4 / 5 (native-USB S3 boards, no UART bridge).
#
#   AIDLINK_PASS=... tools/flash-aid.sh [build-dir] [iface]
#
# The unit must be up and reachable on the cable (its portal answers on :80).
# If it is wedged or unflashed, this cannot help — that is the one case that
# still needs BOOT+RST.
set -euo pipefail

BUILD="${1:-firmware-idf/build}"
IFACE="${2:-}"
BIN="$BUILD/aidlink.bin"
USER="${AIDLINK_USER:-admin}"
PASS="${AIDLINK_PASS:-}"

[ -f "$BIN" ] || { echo "no $BIN — build first"; exit 1; }
# esptool lives in the IDF python env, which is not on PATH in a bare shell.
command -v esptool.py >/dev/null 2>&1 || {
  # shellcheck disable=SC1090
  [ -f "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" ] && . "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" >/dev/null 2>&1
}
command -v esptool.py >/dev/null 2>&1 || { echo "esptool.py not on PATH — source the IDF export.sh"; exit 1; }
[ -n "$PASS" ] || { echo "set AIDLINK_PASS (kept out of the repo deliberately)"; exit 1; }

# --- locate the USB-NCM interface the unit is on -----------------------------
if [ -z "$IFACE" ]; then
  for i in $(ifconfig -a 2>/dev/null | grep -oE '^en[0-9]+'); do
    if ifconfig "$i" 2>/dev/null | grep -q 'inet 172\.20\.'; then IFACE="$i"; break; fi
  done
fi
[ -n "$IFACE" ] || { echo "no USB-NCM interface found (is the cable in?)"; exit 1; }
GW="$(ifconfig "$IFACE" | grep -oE 'inet 172\.20\.[0-9]+\.[0-9]+' | head -1 | awk '{print $2}')"
GW="${GW%.*}.1"
echo "unit at $GW via $IFACE"

COOK="$(mktemp)"; trap 'rm -f "$COOK"' EXIT

# --- authenticate, then force download-boot ----------------------------------
curl -fsS -m 8 --interface "$IFACE" -c "$COOK" -o /dev/null \
     -X POST -d "u=$USER&p=$PASS" "http://$GW/login" || true
code=$(curl -s -m 8 --interface "$IFACE" -b "$COOK" -o /dev/null -w '%{http_code}' "http://$GW/dfu")
[ "$code" = "200" ] || { echo "/dfu returned $code — wrong password, or portal down"; exit 1; }
echo "/dfu accepted — waiting for the ROM downloader"

# --- wait for the ROM port ----------------------------------------------------
PORT=""
for _ in $(seq 1 25); do
  PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
  [ -n "$PORT" ] && break
  sleep 1
done
[ -n "$PORT" ] || { echo "no ROM port appeared — replug the cable and retry"; exit 1; }
echo "downloader on $PORT"

# --- flash --------------------------------------------------------------------
# CRITICAL: --before no_reset. The chip is ALREADY in the downloader; asking
# esptool to reset it again (default_reset / usb_reset) toggles DTR/RTS on the
# USB-Serial-JTAG and is what wedges it into the "port present but silent"
# state. Never reset a chip that is already where you want it.
#
# App slot only. Bootloader and partition table are unchanged between builds,
# and NVS at 0x9000 holds the Wi-Fi credentials — never erase_flash here.
# --no-stub is REQUIRED, not an optimisation. After /dfu the ROM enumerates on
# USB-OTG ("USB mode: USB-OTG" in esptool's banner), and the flasher stub does
# not survive there: it uploads, prints "Stub running...", and then the port goes
# permanently silent — after which nothing but a replug recovers it. Slower, but
# it is the difference between working and needing hands.
esptool.py -p "$PORT" --before no_reset --after hard_reset --no-stub \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x10000 "$BIN"

# --- wait for it to come back -------------------------------------------------
echo "flashed — waiting for AIDlink to come back up"
for _ in $(seq 1 40); do
  if curl -fsS -m 3 --interface "$IFACE" -o /dev/null "http://$GW/" 2>/dev/null; then
    echo "AIDlink is back on $GW"; exit 0
  fi
  sleep 1
done

# The chip can re-latch into forced download boot (boot:0x23) after a flash over
# native USB. esptool's "hard reset" on USB-Serial-JTAG is a USB-level reset, not
# an EN pulse, so it does not always clear that latch.
echo "did not come back — the download latch is probably still set."
echo "tap RST once (no BOOT), or replug the cable."
exit 2
