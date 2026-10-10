// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Capacitive touch panel, read as a second user key. The whole screen is one
// button: a tap and a hold mean exactly what they mean on the physical key
// (button.c), so the touch board needs no gestures of its own.
//
// Presence is decided at runtime: the board profile only says where a
// controller COULD be. If it does not answer at boot, touch_start() returns
// false and the firmware carries on as if the profile had no touch at all.
#pragma once
#include <stdbool.h>

// Reset + identify the controller. False when the profile has none, or it
// did not answer. Safe to call on every target (no-op without the hardware).
bool touch_start(void);

// One poll: is a finger on the glass right now? False when touch is absent
// or a read fails. Cheap enough to call every 20 ms.
bool touch_pressed(void);

// Controller chip ID read at boot (0xB4 CST816S, 0xB5 CST816T, ...), or -1
// when no touch controller answered. For the portal's hardware card.
int touch_chip_id(void);
