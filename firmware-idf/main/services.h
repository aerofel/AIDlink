// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// mDNS advertisement: reachable as <dev_name>.local, advertising the web (:80)
// and ADBP (_aidlink-adbp._tcp) services.
#pragma once
#include "config.h"

void services_start(const aidlink_cfg_t *cfg);
