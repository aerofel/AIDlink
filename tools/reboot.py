#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AIDlink contributors
"""Reboot the ESP32 from the host via an RTS toggle (works on boards whose post-flash auto-reset is flaky).
Usage: python3 tools/reboot.py [port]"""
import sys, time, serial
port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-0001"
s = serial.Serial(port, 115200, timeout=0.3)
s.setDTR(False); s.setRTS(True); time.sleep(0.15); s.setRTS(False)  # EN low->high while GPIO0 high = normal boot
time.sleep(1); s.close()
print("rebooted", port)
