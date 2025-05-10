/*
 * Copyright (C) 2024 Stefan Zabka <stefan@zabka.it>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PSP_X86_CONFIG_SPACE_H
#define PSP_X86_CONFIG_SPACE_H

#include "exec/memory.h"
#include "hw/sysbus.h"

#include "psp/generations.h"

#define TYPE_PSP_X86_CONFIG_SPACE "amd_psp.x86.config_space"
#define PSP_X86_CONFIG_SPACE(obj)                                              \
    OBJECT_CHECK(PSPX86ConfigSpaceState, (obj), TYPE_PSP_X86_CONFIG_SPACE)

#define PCI_CONFIG_SPACE_BASE 0xfffe000UL << 20

/**
 * @brief The PCI Configuration space exposed as MMIO
 *
 * See the PPR NRB 2.1.9 for an explanation of the mapping
 */
typedef struct PSPX86ConfigSpaceState {
    /* <private> */
    SysBusDevice parent;
    /* <public> */
    MemoryRegion iomem;
    PspGeneration generation;
} PSPX86ConfigSpaceState;

#endif // PSP_X86_CONFIG_SPACE_H
