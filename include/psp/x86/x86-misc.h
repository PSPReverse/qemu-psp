/*
 * Copyright (C) 2024 Stefan Zabka <stefan@zabka.it>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef X86_MISC_H
#define X86_MISC_H

#include "system/memory.h"
#include "hw/sysbus.h"

#include "psp/generations.h"
#define TYPE_PSP_X86_MISC "amd_psp.x86.misc"
#define PSP_X86_MISC(obj)                                                      \
    OBJECT_CHECK(PSPX86MiscState, (obj), TYPE_PSP_X86_MISC)

typedef struct PSPX86MiscState {
    /* <private> */
    SysBusDevice parent;
    /* <public> */
    MemoryRegion iomem;
    PspGeneration generation;
    /* I/O port 0x61 placeholder state, see psp_x86_misc_read/write */
    uint8_t port61_ctl;
    bool port61_refresh;
} PSPX86MiscState;

#endif // X86_MISC_H
