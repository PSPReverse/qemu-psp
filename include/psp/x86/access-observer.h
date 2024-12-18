/*
 * Copyright (C) 2024 Stefan Zabka <stefan@zabka.it>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PSP_X86_ACCESS_OBSERVER_H
#define PSP_X86_ACCESS_OBSERVER_H

#include "qemu/osdep.h"

#include "exec/memory.h"
#include "hw/sysbus.h"

#include "psp/generations.h"
#include "psp/x86/psp-x86.h"

#define TYPE_PSP_X86_ACCESS_OBSERVER "amd_psp.x86.access-observer"
#define PSP_X86_ACCESS_OBSERVER(obj)                                           \
    OBJECT_CHECK(AccessObserverState, (obj), TYPE_PSP_X86_ACCESS_OBSERVER)

typedef struct AccessObserverState {
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

    /* Reference to the x86 memory*/
    PSPX86State* x86_memory;

    PspGeneration generation;

} AccessObserverState;

#endif // PSP_X86_ACCESS_OBSERVER_H
