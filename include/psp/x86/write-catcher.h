/*
 * Copyright (C) 2024 Stefan Zabka <stefan@zabka.it>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PSP_X86_WRITE_CATCHER_H
#define PSP_X86_WRITE_CATCHER_H

#include "qemu/osdep.h"

#include "exec/memory.h"
#include "hw/sysbus.h"

#define TYPE_PSP_X86_WRITE_CATCHER "amd_psp.x86.write_catcher"
#define PSP_X86_WRITE_CATCHER(obj)                                             \
    OBJECT_CHECK(WriteCatcherState, (obj), TYPE_PSP_X86_WRITE_CATCHER)

typedef struct WriteCatcherState {
    /* <private> */
    SysBusDevice parent;
    /* <public> */
    MemoryRegion iomem;
    uint64_t size;
    uint64_t base;

    hwaddr observed_illegal_read;
    hwaddr observed_illegal_write;
} WriteCatcherState;

#endif // PSP_X86_WRITE_CATCHER_H
