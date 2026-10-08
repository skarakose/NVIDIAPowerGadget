/*
 * Copyright (C) 2026 Serhat
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NVFAN_H
#define NVFAN_H

#include <stdint.h>

#define NVFAN_MAX_COOLERS 16

typedef struct {
    int32_t temperatureC;
    uint32_t fanRPM[NVFAN_MAX_COOLERS];
    uint32_t fanCount;
} nvfan_status_t;

int nvfan_read_status(nvfan_status_t *status);
uint32_t nvfan_status_fan_rpm(const nvfan_status_t *status, uint32_t index);
int nvfan_set_fan_percent(int percent);
int nvfan_set_auto(void);

#endif
