/*
 * Copyright (C) 2024 Stefan Zabka <stefan@zabka.it>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PSP_X86_WIDE_GENERIC_PORT_H
#define PSP_X86_WIDE_GENERIC_PORT_H

#include "exec/memory.h"
#include "hw/sysbus.h"

#include "psp/generations.h"

#define TYPE_PSP_X86_WIDE_GENERIC_PORT "amd_psp.x86.wide_generic_port"
#define PSP_X86_WIDE_GENERIC_PORT(obj)                                         \
    OBJECT_CHECK(PSPX86WideGenericPortState, (obj),                            \
                 TYPE_PSP_X86_WIDE_GENERIC_PORT)

#define LPC_BASE_ADDRESS 0xfec10000
#define WIDE_GENERIC_PORT_OFFSET 0x1640

typedef struct PSPX86WideGenericPortState {
    /* <private> */
    SysBusDevice parent;
    /* <public> */
    MemoryRegion iomem;
} PSPX86WideGenericPortState;

#endif // PSP_X86_WIDE_GENERIC_PORT_H
