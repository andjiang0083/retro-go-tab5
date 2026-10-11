# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 andjiang0083
idf_component_register(
    SRCS "ppa_engine.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_driver_ppa esp_mm
)
