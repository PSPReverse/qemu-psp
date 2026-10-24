/*
 * AMD PSP emulation
 *
 * Copyright (C) 2020 Robert Buhren <robert@robertbuhren.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef AMD_PSP_X86_H
#define AMD_PSP_X86_H

#include "psp/generations.h"
#include "psp/mmio/psp-sts.h"
#include "psp/x86/config-space.h"
#include "psp/x86/x86-misc.h"

#define TYPE_PSP_X86 "amd_psp.x86"
#define PSP_X86(obj) OBJECT_CHECK(PSPX86State, (obj), TYPE_PSP_X86)

#define X86_PHYS_ADDR_SPACE_SIZE (1UL << 48) /* 48 physical address bits */
/***
 * Manages the x86 address space and all x86 related devices
 */
typedef struct PSPX86State {
    SysBusDevice parent_obj;

    /* PSP Misc device that covers the whole x86 address space */
    PSPX86MiscState psp_x86_misc;

    /*The PCI Config Space also known as PCICFG*/
    PSPX86ConfigSpaceState pci_config_space;

    /* The centralized PSP Status register */
    PSPStsState sts;

    /* The X86 address space */
    AddressSpace as;

    /* The memory region backing the x86 address space */
    MemoryRegion mr;

    /* The Part of the DRAM that gets blocked for exclusive use of the PSP */
    MemoryRegion secure_dram;

    /* DRAM exposed to the host software */
    MemoryRegion regular_dram;

    /* Size of regular_dram in bytes; every snapshot copies the whole block */
    uint64_t regular_dram_size;

    /* The Generation of PSP we are emulating*/
    PspGeneration generation;
} PSPX86State;
#endif
