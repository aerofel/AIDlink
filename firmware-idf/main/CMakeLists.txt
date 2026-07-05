# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AIDlink contributors
idf_component_register(SRCS "aidlink_main.c" "config.c" "config_util.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash)
