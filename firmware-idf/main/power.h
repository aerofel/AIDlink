// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Restart and power-off for the board's power key (board_t.pwr_btn_gpio).
// On a battery the ESP32 is wired straight to the cell (no load switch), so
// "off" is deep sleep with everything else unpowered; a side button wakes it.
#pragma once

// Clean USB detach, then reboot. Never returns.
void power_restart(const char *why);
// Wi-Fi and USB down, display dark and unpowered, deep sleep until a side
// button is pressed. The caller must make sure the buttons are RELEASED: the
// wake is level-triggered, so a held key would wake the board at once.
// Never returns.
void power_off(const char *why);
