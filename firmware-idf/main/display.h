// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Onboard flight display (LilyGO T-Display-S3, ST7789 170x320 on the i80 bus).
// Shows tail, flight number, DEP -> ARR, distance to arrival, and the local
// time / UTC offset at the current position. No-op on boards without a display.
#pragma once
#include "config.h"

void display_start(const aidlink_cfg_t *cfg);
