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
#include "hw/core/sysbus.h"

#include "psp/generations.h"
#include "psp/psp-regs.h"

#define TYPE_PSP_MISC "amd_psp.misc"
#define PSP_MISC(obj) OBJECT_CHECK(PSPMiscState, (obj), TYPE_PSP_MISC)

#define PSP_MISC_MAX_OVERRIDES 16

typedef struct PSPMiscState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    /* Size of MMIO region covered by this instance */
    uint64_t mmio_size;

    /* Identifier of this instance */
    char* ident;

    /* Which Generation is being emulated*/
    /* Log every access with qemu_log_mask(LOG_UNIMP), for builds without tracing */
    bool log_access;
    PspGeneration generation;

    /* Storage of the "rw" entries of psp/regs, see psp_regs_find() */
    uint32_t rw[PSP_REGS_MAX_RW_SLOTS];

    /* "overrides" property: PSP address=value pairs that win on reads */
    char* overrides;
    uint32_t override_addr[PSP_MISC_MAX_OVERRIDES];
    uint32_t override_value[PSP_MISC_MAX_OVERRIDES];
    uint32_t n_overrides;
} PSPMiscState;

#endif
