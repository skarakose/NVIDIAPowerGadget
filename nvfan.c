/*
 * Copyright (C) 2026 Serhat
 * SPDX-License-Identifier: GPL-3.0-or-later
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version. See LICENSE for details.
 */

/*
 * nvfan — NVIDIA fan/temp utility for the NullMoth macOS driver.
 *
 * Talks to NVRM.kext from userspace through the same IOUserClient path NVK
 * uses (NVRMUserClient externalMethod SEL_ESC=1 -> rm_ioctl). No kext change
 * needed for anything the open RM exposes.
 *
 * COMMANDS
 *   nvfan fans            read fan RPM for all coolers via RUSD shared memory
 *   nvfan temp            read GPU temperature (public thermal API)
 *   nvfan status          temp + utilization + fans together
 *   nvfan watch [sec]     poll temp + util + fans every sec (default 1s)
 *   nvfan set <pct>       fixed fan duty for both fans (30-100)
 *   nvfan auto            restore automatic control (rejected on 610 GSP)
 *
 * Build:
 *   clang -O2 -o nvfan nvfan.c -framework IOKit -framework CoreFoundation
 *
 * HOW FAN READING WORKS
 * ─────────────────────
 * The RM exposes a "RM User Shared Data" page: object class NV00DE
 * (RM_USER_SHARED_DATA = 0xDE), allocated under the subdevice, then mapped
 * with NV_ESC_RM_MAP_MEMORY (0x4E) + IOConnectMapMemory64. Fan RPMs live in
 * NV00DE_SHARED_DATA.fanCoolerStatus.info.rpmCurr[] (cl00de.h).
 *
 * Struct layouts match open-gpu-kernel-modules r610
 * (nvos.h, nv-ioctl-numbers.h, ctrl0000gpu.h, ctrl2080thermal.h, cl00de.h).
 */

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "nvfan.h"

/* ── primitive types ──────────────────────────────────────────────────── */
typedef uint8_t  NvU8;  typedef uint16_t NvU16; typedef uint32_t NvU32;
typedef int32_t  NvS32; typedef int32_t  NvV32; typedef uint64_t NvU64;
typedef uint32_t NvHandle; typedef uint64_t NvP64;

/* ── ESC numbers ─────────────────────────────────────────────────────── */
#define NV_ESC_CARD_INFO      200   /* NV_IOCTL_BASE + 0  */
#define NV_ESC_RM_CONTROL     0x2A
#define NV_ESC_RM_ALLOC       0x2B
#define NV_ESC_RM_MAP_MEMORY  0x4E  /* escape: populate fd mmap ctx for MapMemory64 */

/* ── IOUserClient selectors on NVRMUserClient ────────────────────────── */
#define SEL_ESC     1
#define SEL_BINDFD  3

/* ── RM object class IDs ─────────────────────────────────────────────── */
#define CLS_ROOT_CLIENT           0x41u
#define CLS_DEVICE_0              0x80u
#define CLS_SUBDEVICE_0           0x2080u
#define CLS_RM_USER_SHARED_DATA   0x000000DEu  /* NV00DE */

/* ── RM control commands ─────────────────────────────────────────────── */
#define NV0000_CTRL_CMD_GPU_ATTACH_IDS              0x215u
#define NV0000_CTRL_GPU_INVALID_ID                  0xffffffffu
#define NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2   0x20800513u

/* ── thermal opcodes (ctrl2080thermal.h) ─────────────────────────────── */
#define THERM_OP_SENSORS_AVAILABLE  0x00000500u
#define THERM_OP_SENSOR_READING     0x00001500u
#define THERM_API_VER               1u
#define THERM_N_INSTR               0x20u

/* ── RUSD polling mask bits (cl00de.h) ───────────────────────────────── */
#define NV00DE_RUSD_POLL_CLOCK     0x01u
#define NV00DE_RUSD_POLL_PERF      0x02u
#define NV00DE_RUSD_POLL_MEMORY    0x04u
#define NV00DE_RUSD_POLL_POWER     0x08u
#define NV00DE_RUSD_POLL_THERMAL   0x10u
#define NV00DE_RUSD_POLL_PCI       0x20u
#define NV00DE_RUSD_POLL_FAN       0x40u
#define NV00DE_RUSD_POLL_PROC_UTIL 0x80u

/* ── RUSD fan constants ──────────────────────────────────────────────── */
#define RUSD_FAN_COOLER_MAX_COOLERS 16u
#define RUSD_TIMESTAMP_INVALID      0ULL
#define RUSD_TIMESTAMP_WIP          (~0ULL)
#define RUSD_SEQ_START              (0xFF00000000000000ULL)

/*
 * RUSD offsets in NV00DE_SHARED_DATA for the RM the kext runs (610.57.04).
 * Verified with offsetof() against r610 class/cl00de.h AND with live data
 * (0 -> 820 -> 1850 RPM ramp observed on RTX 3060 Ti).
 * Newer trees move theseoffsets, so always re-verify with offsetof()
 * after any version change.
 */
#define RUSD_FAN_STATUS_OFFSET  2552u
#define RUSD_TEMPS_OFFSET       2312u
#define RUSD_UTIL_OFFSET        1992u

/* ── card info struct (nv-ioctl.h) ──────────────────────────────────── */
typedef struct {
    NvU8  valid; NvU8 _p0[3];
    NvU32 domain; NvU8 bus, slot, func, _p1;
    NvU16 vendor_id, device_id;
    NvU32 gpu_id;
    NvU16 interrupt_line; NvU8 _p2[2];
    NvU64 reg_address, reg_size, fb_address, fb_size;
    NvU32 minor_number;
    NvU8  dev_name[10]; NvU8 _p3[2];
} nv_card_info_full_t;

/* ── RM wire structs ─────────────────────────────────────────────────── */
/* NVOS21 — RM alloc */
typedef struct {
    NvHandle hRoot, hObjectParent, hObjectNew;
    NvV32    hClass;
    NvP64    pAllocParms;
    NvU32    paramsSize;
    NvV32    status;
} NVOS21;

/* NVOS54 — RM control */
typedef struct {
    NvHandle hClient, hObject;
    NvV32    cmd;
    NvU32    flags;
    NvP64    params;
    NvU32    paramsSize;
    NvV32    status;
} NVOS54;

/* NVOS33 + fd — map memory: fills the fd's mmap context so
 * IOConnectMapMemory64 can serve the page. */
typedef struct {
    NvHandle hClient;
    NvHandle hDevice;     /* device or subdevice handle */
    NvHandle hMemory;     /* handle to the memory object (NV00DE handle) */
    NvU64    offset   __attribute__((aligned(8)));
    NvU64    length   __attribute__((aligned(8)));
    NvP64    pLinearAddress __attribute__((aligned(8)));  /* out: VA (not used on macOS) */
    NvU32    status;
    NvU32    flags;
} NVOS33;

/* ── NV00DE alloc params ─────────────────────────────────────────────── */
typedef struct {
    NvU64 polledDataMask __attribute__((aligned(8)));
} NV00DE_ALLOC_PARAMS;

/* ── RUSD fan cooler structs (cl00de.h) ──────────────────────────────── */
typedef struct {
    NvU32 rpmCurr[RUSD_FAN_COOLER_MAX_COOLERS];
} RUSD_FAN_COOLER_INFO;

typedef struct {
    volatile NvU64     lastModifiedTimestamp __attribute__((aligned(8)));
    RUSD_FAN_COOLER_INFO info;
} RUSD_FAN_COOLER_STATUS;

/* ── global state ────────────────────────────────────────────────────── */
static io_connect_t gCtl = 0, gDev = 0;
static int gFdCtl = -1;

/* Verbose lines print only with NVFAN_DEBUG=1. Normal runs stay quiet. */
static int g_dbg = -1;
static int dbg(void)
{
    if (g_dbg < 0) {
        const char *e = getenv("NVFAN_DEBUG");
        g_dbg = (e && *e && *e != '0') ? 1 : 0;
    }
    return g_dbg;
}
#define DBG(...) do { if (dbg()) printf(__VA_ARGS__); } while (0)

/* ── low-level RM helpers ────────────────────────────────────────────── */
static int nv_open_one(uint32_t type, io_connect_t *out)
{
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                   IOServiceMatching("NVRM"));
    if (!svc) {
        fprintf(stderr, "nvfan: no NVRM service (driver loaded?)\n");
        return -1;
    }
    io_connect_t c = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), type, &c);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "nvfan: IOServiceOpen(type=%u) -> 0x%x\n", type, kr);
        return -1;
    }
    int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    uint64_t in = (uint64_t)fd;
    kr = IOConnectCallScalarMethod(c, SEL_BINDFD, &in, 1, NULL, NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "nvfan: BINDFD -> 0x%x\n", kr);
        IOServiceClose(c);
        return -1;
    }
    *out = c;
    if (type == 0) gFdCtl = fd;
    return 0;
}

static void nv_close_all(void)
{
    if (gCtl) { IOServiceClose(gCtl); gCtl = 0; }
    if (gDev) { IOServiceClose(gDev); gDev = 0; }
    if (gFdCtl >= 0) { close(gFdCtl); }
    gFdCtl = -1;
}

static int nv_esc(io_connect_t c, uint32_t cmd, void *data, uint32_t size)
{
    uint64_t in = cmd;
    size_t out = size;
    kern_return_t kr = IOConnectCallMethod(c, SEL_ESC, &in, 1, data, size,
                                           NULL, NULL, data, &out);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "nvfan: esc 0x%x -> kr 0x%x\n", cmd, kr);
        return -1;
    }
    return 0;
}

static NvHandle rm_alloc(NvHandle root, NvHandle parent, NvU32 cls,
                          void *par, NvU32 parSize)
{
    NVOS21 a; memset(&a, 0, sizeof a);
    a.hRoot          = root;
    a.hObjectParent  = parent;
    a.hObjectNew     = 0;
    a.hClass         = (NvV32)cls;
    a.pAllocParms    = par ? (NvP64)(uintptr_t)par : 0;
    a.paramsSize     = parSize;
    if (nv_esc(gCtl, NV_ESC_RM_ALLOC, &a, sizeof a)) return 0;
    if (a.status != 0) {
        fprintf(stderr, "nvfan: ALLOC class 0x%x -> status 0x%x\n",
                cls, (unsigned)a.status);
        return 0;
    }
    return a.hObjectNew;
}

static NvS32 rm_control(NvHandle client, NvHandle obj, NvU32 cmd,
                         void *par, NvU32 parSize)
{
    NVOS54 c; memset(&c, 0, sizeof c);
    c.hClient    = client;
    c.hObject    = obj;
    c.cmd        = (NvV32)cmd;
    c.params     = (NvP64)(uintptr_t)par;
    c.paramsSize = parSize;
    if (nv_esc(gCtl, NV_ESC_RM_CONTROL, &c, sizeof c)) return -1;
    return c.status;
}

/*
 * Map a memory object into userspace.
 * Sends NV_ESC_RM_MAP_MEMORY (0x4E) with nv_ioctl_nvos33_parameters_with_fd
 * (NVOS33 + int fd) to populate the fd's mmap context,
 * then calls IOConnectMapMemory64 to get the VA.
 */
static void *rm_map_memory(NvHandle hClient, NvHandle hDevice, NvHandle hMemory,
                            NvU64 offset, NvU64 length, mach_vm_size_t *mapped_size)
{
    struct { NVOS33 p; int fd; } w; memset(&w, 0, sizeof w);
    w.p.hClient  = hClient;
    w.p.hDevice  = hDevice;
    w.p.hMemory  = hMemory;
    w.p.offset   = offset;
    w.p.length   = length;
    w.p.flags    = 1; /* NVOS33_FLAGS_ACCESS_READ_ONLY */
    w.fd         = gFdCtl;

    /* This fills the ctl fd's mmap context so IOConnectMapMemory64 works. */
    if (nv_esc(gCtl, NV_ESC_RM_MAP_MEMORY, &w, sizeof w)) return NULL;
    if (w.p.status != 0) {
        fprintf(stderr, "nvfan: MAP_MEMORY -> status 0x%x\n", (unsigned)w.p.status);
        return NULL;
    }

    /* Now map the populated mmap context into our address space */
    mach_vm_address_t addr = 0;
    mach_vm_size_t    msz  = (mach_vm_size_t)length;
    kern_return_t kr = IOConnectMapMemory64(gCtl, 0,
                                             mach_task_self(), &addr, &msz,
                                             kIOMapAnywhere | kIOMapReadOnly);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "nvfan: IOConnectMapMemory64 -> 0x%x\n", kr);
        return NULL;
    }

    if (mapped_size) *mapped_size = msz;
    return (void *)(uintptr_t)addr;
}

/* Bring up client/device/subdevice handles. Returns subdevice, 0 on error. */
static NvHandle rm_bringup(NvHandle *pClient, NvHandle *pDevice, NvU32 *pGpuId)
{
    nv_card_info_full_t ci; memset(&ci, 0, sizeof ci);
    if (nv_esc(gCtl, NV_ESC_CARD_INFO, &ci, sizeof ci) || !ci.valid) {
        fprintf(stderr, "nvfan: no card info (adapter up? ran `go 2`?)\n");
        return 0;
    }

    NvHandle hClient = rm_alloc(0, 0, CLS_ROOT_CLIENT, NULL, 0);
    if (!hClient) return 0;

    struct { NvU32 ids[32]; NvU32 failed; } at;
    memset(&at, 0, sizeof at);
    at.ids[0] = ci.gpu_id; at.ids[1] = NV0000_CTRL_GPU_INVALID_ID;
    NvS32 st = rm_control(hClient, hClient,
                          NV0000_CTRL_CMD_GPU_ATTACH_IDS, &at, sizeof at);
    if (st != 0) {
        fprintf(stderr, "nvfan: ATTACH_IDS -> 0x%x\n", (unsigned)st);
        return 0;
    }

    struct { NvU32 devId; NvHandle share, tCli, tDev; NvV32 flags;
             NvU64 vaSize, vaStart, vaLimit; NvV32 vaMode; } dp;
    memset(&dp, 0, sizeof dp);
    dp.devId = 0; dp.share = hClient;
    NvHandle hDevice = rm_alloc(hClient, hClient, CLS_DEVICE_0, &dp, sizeof dp);
    if (!hDevice) return 0;

    NvU32 sub = 0;
    NvHandle hSub = rm_alloc(hClient, hDevice, CLS_SUBDEVICE_0, &sub, sizeof sub);
    if (!hSub) return 0;

    *pClient = hClient; *pDevice = hDevice;
    if (pGpuId) *pGpuId = ci.gpu_id;
    return hSub;
}

/* ── temperature via thermal instruction API ────────────────────────── */
typedef struct { NvU32 result, executed, opcode; NvU32 ops[8]; } ThermInstr;
typedef struct { NvU32 ver, rev, instrSize, flags, okCount, nInstr;
                 ThermInstr list[THERM_N_INSTR]; } ThermExec;

static int read_temperature(NvHandle hClient, NvHandle hSub,
                             int *tempC_out, int *nSensors_out)
{
    ThermExec t; memset(&t, 0, sizeof t);
    t.ver = THERM_API_VER; t.instrSize = sizeof(ThermInstr);
    t.nInstr = 2;
    t.list[0].opcode = THERM_OP_SENSORS_AVAILABLE;
    t.list[1].opcode = THERM_OP_SENSOR_READING;
    t.list[1].ops[0] = 0; /* sensorIndex 0 */
    NvS32 st = rm_control(hClient, hSub,
                          NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2,
                          &t, sizeof t);
    if (st != 0) return -1;
    if (nSensors_out && t.list[0].executed)
        *nSensors_out = (int)t.list[0].ops[0];
    if (tempC_out && t.list[1].executed)
        *tempC_out = (int)(NvS32)t.list[1].ops[1];
    return (t.list[1].executed && t.list[1].result == 0) ? 0 : -1;
}

/* NV00DE is allocated under the subdevice, then mapped. */
static void *rusd_map_page(NvHandle hClient, NvHandle hDevice, NvHandle hSub,
                          mach_vm_size_t *msz_out)
{
    /* Alloc NV00DE under hSubdevice with fan+thermal+perf polling */
    NV00DE_ALLOC_PARAMS ap; memset(&ap, 0, sizeof ap);
    ap.polledDataMask = (NvU64)(NV00DE_RUSD_POLL_FAN | NV00DE_RUSD_POLL_THERMAL |
                                NV00DE_RUSD_POLL_PERF);

    NvHandle hRusd = rm_alloc(hClient, hSub,
                               CLS_RM_USER_SHARED_DATA, &ap, sizeof ap);
    if (!hRusd) return NULL;

    /* Map the NV00DE memory object — exact RUSD size for r610 is 3152 B */
    mach_vm_size_t msz = 0;
    void *page = rm_map_memory(hClient, hDevice, hRusd, 0, 3152, &msz);
    if (!page) {
        fprintf(stderr, "nvfan: RUSD page map failed\n");
        return NULL;
    }
    if (msz < RUSD_FAN_STATUS_OFFSET + sizeof(RUSD_FAN_COOLER_STATUS)) {
        fprintf(stderr, "nvfan: RUSD page too small (%llu B)\n",
                (unsigned long long)msz);
        IOConnectUnmapMemory64(gCtl, 0, mach_task_self(),
                               (mach_vm_address_t)(uintptr_t)page);
        return NULL;
    }
    *msz_out = msz;
    return page;
}

static int rusd_read_fans(const void *page,
                           uint32_t rpm_out[RUSD_FAN_COOLER_MAX_COOLERS],
                           int *nFans_out);

/* GPU/memory utilization % from an already-mapped RUSD page (610 @1992). */
static int rusd_read_util(const void *page, uint32_t *gpuB, uint32_t *memB)
{
    uint8_t buf[48];
    const volatile uint8_t *b = (const volatile uint8_t *)page;
    for (int i = 0; i < 10; i++) {
        uint64_t t0;
        memcpy(&t0, (const void *)(b + RUSD_UTIL_OFFSET), 8);
        __asm__ volatile("" ::: "memory");
        if (t0 == RUSD_TIMESTAMP_INVALID || t0 == RUSD_TIMESTAMP_WIP) { usleep(2000); continue; }
        memcpy(buf, (const void *)(b + RUSD_UTIL_OFFSET), sizeof buf);
        __asm__ volatile("" ::: "memory");
        uint64_t t1;
        memcpy(&t1, (const void *)(b + RUSD_UTIL_OFFSET), 8);
        if (t0 == t1) {
            memcpy(gpuB, buf + 8, 4); memcpy(memB, buf + 12, 4);
            return 0;
        }
    }
    return -1;
}

/* One-shot fan read: map, read, unmap. */
static int read_fans_rusd(NvHandle hClient, NvHandle hDevice, NvHandle hSub,
                           uint32_t rpm_out[RUSD_FAN_COOLER_MAX_COOLERS],
                           int *nFans_out)
{
    mach_vm_size_t msz = 0;
    void *page = rusd_map_page(hClient, hDevice, hSub, &msz);
    if (!page) return -1;

    int rc = rusd_read_fans(page, rpm_out, nFans_out);
    IOConnectUnmapMemory64(gCtl, 0, mach_task_self(),
                           (mach_vm_address_t)(uintptr_t)page);
    return rc;
}

/* Seqlock read of fan RPMs from an already-mapped RUSD page. */
static int rusd_read_fans(const void *page,
                           uint32_t rpm_out[RUSD_FAN_COOLER_MAX_COOLERS],
                           int *nFans_out)
{
    const volatile RUSD_FAN_COOLER_STATUS *pfcs =
        (const volatile RUSD_FAN_COOLER_STATUS *)
        ((uintptr_t)page + RUSD_FAN_STATUS_OFFSET);

    /* Seqlock-style read: retry if timestamp changes mid-read */
    RUSD_FAN_COOLER_STATUS fcs;
    int retries = 20, ok = 0;
    while (retries-- > 0) {
        NvU64 ts1 = pfcs->lastModifiedTimestamp;
        __asm__ volatile("" ::: "memory");
        memcpy(&fcs, (const void *)pfcs, sizeof fcs);
        __asm__ volatile("" ::: "memory");
        NvU64 ts2 = pfcs->lastModifiedTimestamp;

        if (ts1 == RUSD_TIMESTAMP_INVALID) {
            /* Not yet populated — wait a bit and retry */
            usleep(10000);
            continue;
        }
        if (ts1 == ts2 &&
            ts1 != RUSD_TIMESTAMP_WIP &&
            !(ts1 >= RUSD_SEQ_START && (ts1 & 1ULL))) {
            ok = 1;
            break;
        }
        usleep(1000);
    }

    /* NOTE: caller owns the page — do NOT unmap here. */

    if (!ok) {
        fprintf(stderr, "nvfan: RUSD timestamp unstable or data never written "
                "(GPU busy or fan data unsupported)\n");
        return -1;
    }

    int n = 0;
    for (unsigned i = 0; i < RUSD_FAN_COOLER_MAX_COOLERS; i++) {
        rpm_out[i] = fcs.info.rpmCurr[i];
        if (fcs.info.rpmCurr[i] > 0) n++;
    }
    if (nFans_out) *nFans_out = n;
    return 0;
}

/* ── cooler GET/SET (private RM commands, found via Linux NVML trace) ──
 *   GET = 0x20808530 (980 B), SET = 0x2080c531 (980 B), target = subdevice.
 *   Duty = u32 LE, low 16 bits = round(pct/100 * 0xFFFF):
 *     fan0 duty @0x28, fan1 duty @0x64.
 *   Templates below are verbatim NVML SET buffers from
 *   linux dkms trace results for RTX 3060 Ti, 2 fans; only the duty
 *   words are patched. On the 610 GSP the op-01 policy SET is unimplemented
 *   (status 0x1e) but the op-03 speed SET works standalone from AUTO state.
 */
/* SMC sessions (seen in the same trace): AMPERE_SMC_CONFIG_SESSION (0xc639)
 * + AMPERE_SMC_MONITOR_SESSION (0xc640), allocated under the root client.
 * Their params are RS_REQUIRED: NULL (size 0) fails with 0x1f, an 8-byte
 * zeroed struct allocates fine. */
#define CLS_SMC_CONFIG   0xc639u
#define CLS_SMC_MONITOR  0xc640u
#define NV2080_CTRL_CMD_COOLER_GET  0x20808530u
#define NV2080_CTRL_CMD_COOLER_SET  0x2080c531u
#define COOLER_BUF 980u
#define COOLER_DUTY0_OFF 0x28u
#define COOLER_DUTY1_OFF 0x64u

/* SPEED: NVML SET template, 980 B */
static const uint8_t TPL_SPEED[980] = {
    0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x20, 0x08, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x66, 0xa6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x85, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x08, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x85, 0x0b, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

/* AUTO: NVML SET template, 980 B */
static const uint8_t TPL_AUTO[980] = {
    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x20, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x85, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

static int cooler_get(NvHandle hClient, NvHandle hSub, uint8_t *buf980)
{
    memset(buf980, 0, COOLER_BUF);
    NvS32 st = rm_control(hClient, hSub, NV2080_CTRL_CMD_COOLER_GET,
                           buf980, COOLER_BUF);
    return (int)st;
}

static int cooler_set_raw(NvHandle hClient, NvHandle hSub, const uint8_t *buf980)
{
    /* rm_control takes non-const; cast is safe (RM only reads for SET). */
    NvS32 st = rm_control(hClient, hSub, NV2080_CTRL_CMD_COOLER_SET,
                           (void *)buf980, COOLER_BUF);
    return (int)st;
}

/* ── sub-commands ───────────────────────────────────────────────────── */

static int cmd_fans(void)
{
    if (nv_open_one(0, &gCtl) || nv_open_one(1, &gDev)) return 1;

    NvHandle hClient = 0, hDevice = 0; NvU32 gpuId = 0;
    NvHandle hSub = rm_bringup(&hClient, &hDevice, &gpuId);
    if (!hSub) return 1;

    printf("GPU 0x%x  client 0x%x  device 0x%x  subdevice 0x%x\n",
           gpuId, hClient, hDevice, hSub);

    uint32_t rpm[RUSD_FAN_COOLER_MAX_COOLERS] = {0};
    int nFans = 0;
    if (read_fans_rusd(hClient, hDevice, hSub, rpm, &nFans) != 0) {
        fprintf(stderr, "nvfan: fan speed read failed\n");
        return 1;
    }

    printf("Fan coolers detected: %d (of %d max)\n", nFans, RUSD_FAN_COOLER_MAX_COOLERS);
    for (unsigned i = 0; i < RUSD_FAN_COOLER_MAX_COOLERS; i++)
        if (rpm[i] > 0)
            printf("  cooler %2u: %5u RPM\n", i, rpm[i]);
    if (nFans == 0)
        printf("  (no fans spinning or GPU has no fans)\n");
    return 0;
}

static int cmd_temp(void)
{
    if (nv_open_one(0, &gCtl) || nv_open_one(1, &gDev)) return 1;

    NvHandle hClient = 0, hDevice = 0; NvU32 gpuId = 0;
    NvHandle hSub = rm_bringup(&hClient, &hDevice, &gpuId);
    if (!hSub) return 1;

    printf("GPU 0x%x  client 0x%x  device 0x%x  subdevice 0x%x\n",
           gpuId, hClient, hDevice, hSub);

    int tempC = 0, nSensors = 0;
    if (read_temperature(hClient, hSub, &tempC, &nSensors) != 0) {
        fprintf(stderr, "nvfan: temperature read failed\n");
        return 1;
    }
    printf("Thermal sensors: %d\n", nSensors);
    printf("GPU temperature: %d °C\n", tempC);
    return 0;
}

static int cmd_status(void)
{
    if (nv_open_one(0, &gCtl) || nv_open_one(1, &gDev)) return 1;

    NvHandle hClient = 0, hDevice = 0; NvU32 gpuId = 0;
    NvHandle hSub = rm_bringup(&hClient, &hDevice, &gpuId);
    if (!hSub) return 1;

    printf("GPU 0x%x\n", gpuId); fflush(stdout);

    int tempC = 0, nSensors = 0;
    if (read_temperature(hClient, hSub, &tempC, &nSensors) == 0)
        printf("  Temperature : %d °C  (%d sensor%s)\n",
               tempC, nSensors, nSensors == 1 ? "" : "s");
    else
        printf("  Temperature : unavailable\n");

    uint32_t rpm[RUSD_FAN_COOLER_MAX_COOLERS] = {0};
    int nFans = 0;
    uint32_t ug = 0, um = 0;
    mach_vm_size_t msz = 0;
    void *pg = rusd_map_page(hClient, hDevice, hSub, &msz);
    int frc = -1;
    if (pg) {
        frc = rusd_read_fans(pg, rpm, &nFans);
        if (rusd_read_util(pg, &ug, &um) == 0)
            printf("  Utilization : gpu %u%%  mem %u%%\n", ug, um);
        IOConnectUnmapMemory64(gCtl, 0, mach_task_self(),
                               (mach_vm_address_t)(uintptr_t)pg);
    }
    if (frc == 0) {
        if (nFans > 0) {
            for (unsigned i = 0; i < RUSD_FAN_COOLER_MAX_COOLERS; i++) {
                if (rpm[i] > 0) {
                    printf("  Fan cooler %u: %u RPM\n", i, rpm[i]);
                }
            }
        } else {
            printf("  Fans        : no spinning fans (GPU may be passively cooled)\n");
        }
    } else {
        printf("  Fans        : read failed\n");
    }
    return 0;
}

static int cmd_watch(int interval_sec)
{
    if (interval_sec < 1) interval_sec = 1;

    printf("Watching GPU every %ds. Ctrl-C to stop.\n", interval_sec);
    printf("%-8s  %-4s %-4s  %s\n", "Time", "Temp", "Util", "Fan RPM");
    printf("--------  ---- -----  --------\n");

    /* NOTE: fresh IOService session per sample. The RUSD mapping goes
     * stale after one sampling round and the kext wants a fresh fd
     * for each NV_ESC_RM_MAP_MEMORY, so reusing one session crashes. */
    for (;;) {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char tbuf[16]; strftime(tbuf, sizeof tbuf, "%H:%M:%S", tm);

        int tempC = 0;
        uint32_t rpm[RUSD_FAN_COOLER_MAX_COOLERS];
        int nFans = 0;
        uint32_t ug = 0;
        memset(rpm, 0, sizeof rpm);
        if (nv_open_one(0, &gCtl) == 0) {
            NvHandle hClient = 0, hDevice = 0;
            NvHandle hSub = rm_bringup(&hClient, &hDevice, NULL);
            if (hSub) {
                read_temperature(hClient, hSub, &tempC, NULL);
                mach_vm_size_t msz = 0;
                void *pg = rusd_map_page(hClient, hDevice, hSub, &msz);
                if (pg) {
                    uint32_t um = 0;
                    rusd_read_fans(pg, rpm, &nFans);
                    if (rusd_read_util(pg, &ug, &um) != 0) ug = 0;
                    IOConnectUnmapMemory64(gCtl, 0, mach_task_self(),
                                           (mach_vm_address_t)(uintptr_t)pg);
                }
            }
            nv_close_all();
        }

        printf("%s  %-4d %-3u%%  ", tbuf, tempC, ug);
        if (nFans == 0) {
            printf("(none)");
        } else {
            int first = 1;
            for (unsigned i = 0; i < RUSD_FAN_COOLER_MAX_COOLERS; i++) {
                if (rpm[i] > 0) {
                    if (!first) printf(", ");
                    printf("fan%u=%u", i, rpm[i]);
                    first = 0;
                }
            }
        }
        printf("\n");
        fflush(stdout);
        sleep((unsigned)interval_sec);
    }
    return 0;
}

static void smc_bringup(NvHandle hClient)
{
    /* NVC639/640 params are RS_REQUIRED: NULL (size 0) -> 0x1f.
     * An 8-byte zeroed struct (capDescriptor=0) allocates fine. */
    uint64_t zero = 0;
    NvHandle cfg = rm_alloc(hClient, hClient, CLS_SMC_CONFIG, &zero, 8);
    NvHandle mon = rm_alloc(hClient, hClient, CLS_SMC_MONITOR, &zero, 8);
    DBG("SMC sessions: config=0x%x monitor=0x%x\n", cfg, mon);
}

/* Manual fan speed. Both fans get the same duty; 30-100% enforced. */
static int cmd_set(int pct)
{
    if (pct < 30 || pct > 100) {
        fprintf(stderr, "nvfan set: pct must be 30-100 (refusing: %d)\n", pct);
        return 2;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "nvfan set: needs root since driver 2.0 (RM administrator check) — retry with sudo\n");
        return 1;
    }
    if (nv_open_one(0, &gCtl) || nv_open_one(1, &gDev)) return 1;
    NvHandle hClient = 0, hDevice = 0; NvU32 gpuId = 0;
    NvHandle hSub = rm_bringup(&hClient, &hDevice, &gpuId);
    if (!hSub) return 1;
    smc_bringup(hClient);

    static uint8_t get[COOLER_BUF];
    int st = cooler_get(hClient, hSub, get);
    DBG("COOLER_GET sanity: status 0x%x\n", st);
    if (st != 0) {
        fprintf(stderr, "nvfan set: cooler GET failed — RM does not speak "
                "this command here. Aborting, nothing changed.\n");
        return 1;
    }

    /* NOTE: no MANUAL-policy step. Op-01 (TPL_MANUAL) is not implemented
     * in the 610 GSP firmware (status 0x1e); the op-03 speed SET below
     * works standalone, verified live from AUTO state. */
    static uint8_t speed[COOLER_BUF];
    memcpy(speed, TPL_SPEED, sizeof speed);
    uint32_t duty = (uint32_t)((pct * 65535u + 50u) / 100u);
    speed[COOLER_DUTY0_OFF + 0] = (uint8_t)(duty & 0xff);
    speed[COOLER_DUTY0_OFF + 1] = (uint8_t)((duty >> 8) & 0xff);
    speed[COOLER_DUTY0_OFF + 2] = 0; speed[COOLER_DUTY0_OFF + 3] = 0;
    speed[COOLER_DUTY1_OFF + 0] = (uint8_t)(duty & 0xff);
    speed[COOLER_DUTY1_OFF + 1] = (uint8_t)((duty >> 8) & 0xff);
    speed[COOLER_DUTY1_OFF + 2] = 0; speed[COOLER_DUTY1_OFF + 3] = 0;
    speed[0x60] = 1; /* as in traced buffers with nonzero duty1; meaning unknown */
    st = cooler_set_raw(hClient, hSub, speed);
    DBG("SPEED %d%% (duty 0x%04x) SET: status 0x%x\n", pct, duty & 0xffff, st);
    if (st != 0) { fprintf(stderr, "nvfan set: SPEED step failed.\n"); return 1; }

    printf("fans set to %d%%\n", pct);
    return 0;
}

static int cmd_auto(void)
{
    if (geteuid() != 0) {
        fprintf(stderr, "nvfan auto: needs root since driver 2.0 (RM administrator check) — retry with sudo\n");
        return 1;
    }
    if (nv_open_one(0, &gCtl) || nv_open_one(1, &gDev)) return 1;
    NvHandle hClient = 0, hDevice = 0; NvU32 gpuId = 0;
    NvHandle hSub = rm_bringup(&hClient, &hDevice, &gpuId);
    if (!hSub) return 1;
    smc_bringup(hClient);
    static uint8_t get[COOLER_BUF];
    int st = cooler_get(hClient, hSub, get);
    DBG("COOLER_GET sanity: status 0x%x\n", st);
    if (st != 0) { fprintf(stderr, "nvfan auto: cooler GET failed, aborting.\n"); return 1; }
    st = cooler_set_raw(hClient, hSub, TPL_AUTO);
    DBG("AUTO policy SET: status 0x%x\n", st);
    if (st != 0) { fprintf(stderr, "nvfan auto: rejected (status 0x%x).\n", st); return 1; }
    printf("fan control: auto\n");
    return 0;
}

int nvfan_read_status(nvfan_status_t *status)
{
    if (!status) return -1;
    memset(status, 0, sizeof(*status));

    if (nv_open_one(0, &gCtl) != 0) return -1;

    NvHandle hClient = 0, hDevice = 0;
    NvHandle hSub = rm_bringup(&hClient, &hDevice, NULL);
    if (!hSub) {
        nv_close_all();
        return -1;
    }

    int temperature = 0;
    if (read_temperature(hClient, hSub, &temperature, NULL) != 0) {
        nv_close_all();
        return -1;
    }
    status->temperatureC = temperature;

    mach_vm_size_t mappedSize = 0;
    void *page = rusd_map_page(hClient, hDevice, hSub, &mappedSize);
    if (!page) {
        nv_close_all();
        return -1;
    }

    int fanCount = 0;
    int result = rusd_read_fans(page, status->fanRPM, &fanCount);
    IOConnectUnmapMemory64(gCtl, 0, mach_task_self(),
                           (mach_vm_address_t)(uintptr_t)page);
    nv_close_all();
    if (result != 0) return -1;

    status->fanCount = (uint32_t)fanCount;
    return 0;
}

uint32_t nvfan_status_fan_rpm(const nvfan_status_t *status, uint32_t index)
{
    if (!status || index >= NVFAN_MAX_COOLERS) return 0;
    return status->fanRPM[index];
}

int nvfan_set_fan_percent(int percent)
{
    int result = cmd_set(percent);
    nv_close_all();
    return result;
}

int nvfan_set_auto(void)
{
    int result = cmd_auto();
    nv_close_all();
    return result;
}

/* ── main ───────────────────────────────────────────────────────────── */
#ifndef NVFAN_LIBRARY
int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "fans"))   return cmd_fans();
    if (argc >= 2 && !strcmp(argv[1], "temp"))   return cmd_temp();
    if (argc >= 2 && !strcmp(argv[1], "status")) return cmd_status();
    if (argc >= 2 && !strcmp(argv[1], "watch")) {
        int sec = (argc >= 3) ? atoi(argv[2]) : 1;
        return cmd_watch(sec);
    }
    if (argc >= 3 && !strcmp(argv[1], "set")) return cmd_set(atoi(argv[2]));
    if (argc >= 2 && !strcmp(argv[1], "auto")) return cmd_auto();
    printf("nvfan — NVIDIA GPU fan/temp tool (NullMoth macOS driver)\n\n"
           "Usage:\n"
           "  nvfan fans            read RPM for all fans\n"
           "  nvfan temp            read GPU temperature\n"
           "  nvfan status          read fans + temp together\n"
           "  nvfan watch [sec]     continuously poll (default: 1s)\n"
           "  nvfan set <pct>       manual fan speed, both fans (30-100)\n"
           "  nvfan auto            back to automatic fan control\n"
);
    return 2;
}
#endif
