
/*
 * Copyright (C) 2024 Stefan Zabka <stefan@zabka.it>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PSP_X86_WRITE_CATCHER_H
#define PSP_X86_WRITE_CATCHER_H

#include "exec/memory.h"
#include "hw/sysbus.h"

#include "psp/generations.h"
#include "psp/x86/psp-x86.h"

#define TYPE_PSP_X86_WRITE_CATCHER "amd_psp.x86.write_catcher"
#define PSP_X86_WRITE_CATCHER(obj)                                             \
    OBJECT_CHECK(WriteCatcherState, (obj), TYPE_PSP_X86_WRITE_CATCHER)

typedef struct WriteCatcherState {
    /* Base Class */
    SysBusDevice parent;
    /* Fields */
    hwaddr size;
    hwaddr base;

    /* Tracking Results */
    hwaddr observed_illegal_read;
    vaddr pc_illegal_read;
    hwaddr observed_illegal_write;
    vaddr pc_illegal_write;

    /* Intermediate Buffer*/
    MemoryRegion mr;

    /* Manages the underlying address space and all devices mounted into it*/
    PSPX86State x86_memory;

    PspGeneration generation;

} WriteCatcherState;

#endif // PSP_X86_WRITE_CATCHER_H
