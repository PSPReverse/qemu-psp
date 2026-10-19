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
#ifndef AMD_PSP_MISC_H
#define AMD_PSP_MISC_H
#include "system/memory.h"
#include "hw/sysbus.h"

#include "psp/generations.h"

#define TYPE_PSP_MISC "amd_psp.misc"
#define PSP_MISC(obj) OBJECT_CHECK(PSPMiscState, (obj), TYPE_PSP_MISC)

/* Registers that are accessed but not modelled (psp-misc.c) */
#define PSP_MISC_ZEN3_PLACEHOLDER_COUNT 10
#define PSP_MISC_PLACEHOLDER_COUNT 6

typedef struct PSPMiscState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    /* Size of MMIO region covered by this instance */
    uint64_t mmio_size;

    /* Identifier of this instance */
    char* ident;

    /* Which Generation is being emulated*/
    PspGeneration generation;

    /* Zen3 placeholder registers, see psp_misc_placeholder_slot() */
    uint32_t zen3_placeholder[PSP_MISC_ZEN3_PLACEHOLDER_COUNT];

    /* Placeholder registers on all generations */
    uint32_t placeholder[PSP_MISC_PLACEHOLDER_COUNT];

} PSPMiscState;

#endif
