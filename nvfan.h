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

/* Extended monitor (FB / per-engine UTIL / clocks). Fields are -1 when
 * the query is unavailable; vram values are MiB, clocks are MHz. */
typedef struct {
    int32_t vramUsedMB;
    int32_t vramTotalMB;
    int32_t utilGE;
    int32_t utilFB;
    int32_t utilNVENC;
    int32_t utilNVDEC;
    int32_t gpuClockMHz;
    int32_t memClockMHz;
} nvfan_extended_t;

int nvfan_read_extended(nvfan_extended_t *out);

#endif
