/*
 * Copyright (C) 2024 Stefan Zabka <stefan@zabka.it>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PSP_X86_ACCESS_OBSERVER_H
#define PSP_X86_ACCESS_OBSERVER_H

#include "system/memory.h"
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

    /*
     * Overlay mode: instead of the full-space region above, an I/O region
     * shadows just [base, base + size) of the x86 memory while a window is
     * active. It forwards to the RAM through an alias-only copy of the x86
     * memory.
     */
    bool overlay;
    bool overlay_mapped;
    MemoryRegion overlay_mr;
    MemoryRegion backing_mr;
    MemoryRegion secure_alias;
    MemoryRegion regular_alias;
    AddressSpace backing_as;

    PspGeneration generation;

} AccessObserverState;

#endif // PSP_X86_ACCESS_OBSERVER_H
