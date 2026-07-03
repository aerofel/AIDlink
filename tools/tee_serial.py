#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AIDlink contributors
"""Background serial logger for AIDlink.

Opens the USB serial port ONCE (the board auto-resets once on open, then stays up), holds it open, sends
'L' to dump the firmware's in-RAM ring buffer, and tees every subsequent line to a local file. Because the
firmware mirrors its traffic log to serial, this file is the complete, always-current device log — read it
on demand, no reboot. Stop it before flashing (tools/flash.sh does this).

Usage: python3 tools/tee_serial.py [port]   (default /dev/cu.usbserial-0001)
"""
import sys, time, serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-0001"
OUT = "aidlink_serial.log"

while True:
    try:
        s = serial.Serial(PORT, 115200, timeout=1)
    except Exception:
        time.sleep(2); continue
    with open(OUT, "w", buffering=1) as f:
        f.write("===== logger attached %s (port %s) =====\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), PORT))
        try: s.write(b"L\n")   # ask firmware to dump its ring-buffer history
        except Exception: pass
        while True:
            try:
                d = s.readline().decode("utf-8", "replace")
            except Exception:
                break
            if d:
                f.write(d)
    try: s.close()
    except Exception: pass
    time.sleep(2)   # port vanished (unplug/flash) -> wait and reattach
