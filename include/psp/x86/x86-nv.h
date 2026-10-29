/*
 * AMD PSP Zen3 non-volatile store, served by the x86 host (FCH SMI trigger)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef X86_NV_H
#define X86_NV_H

#include "system/memory.h"
#include "hw/sysbus.h"

#define TYPE_PSP_X86_NV "amd_psp.x86.nv"
#define PSP_X86_NV(obj) OBJECT_CHECK(PSPX86NvState, (obj), TYPE_PSP_X86_NV)

/* x86 addresses of the two FCH registers the BL consults */
#define PSP_X86_NV_SMI_TRIGGER 0xfed80298UL
#define PSP_X86_NV_SMI_STATUS 0xfed802a8UL

#define PSP_X86_NV_BLOCK_SIZE 0x1000
#define PSP_X86_NV_BLOCKS 8

typedef struct PSPX86NvState {
    /* <private> */
    SysBusDevice parent;
    /* <public> */
    MemoryRegion trigger;
    MemoryRegion status;
    AddressSpace *as;           /* x86 address space, set by the owner */

    /* Properties */
    bool enabled;
    uint64_t buffer;            /* x86 address of the shared buffer */

    /* State */
    uint8_t store[PSP_X86_NV_BLOCK_SIZE * PSP_X86_NV_BLOCKS];
} PSPX86NvState;

#endif // X86_NV_H
