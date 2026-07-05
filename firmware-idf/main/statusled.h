// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Onboard addressable RGB LED (WS2812) as a Wi-Fi status indicator (ESP32-S3).
// No-op on targets without the onboard RGB LED.
#pragma once

void statusled_start(void);
