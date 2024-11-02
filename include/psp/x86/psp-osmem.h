
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
#ifndef AMD_PSP_OSMEM_H
#define AMD_PSP_OSMEM_H

#include "exec/memory.h"
#include "hw/sysbus.h"

#define TYPE_PSP_OSMEM "amd_psp.osmem"
#define PSP_OSMEM(obj) OBJECT_CHECK(PSPOSMemState, (obj), TYPE_PSP_OSMEM)

#define PSP_OSMEM_NUM_SLOTS 62

/* PSP OSMem iomem size */
#define PSP_OSMEM_CTL_SLOT_SIZE ((uint64_t)0x10)
#define PSP_OSMEM_CONTAINER_SLOT_SIZE ((uint64_t)0x04000000)

#define PSP_OSMEM_CTL_SIZE (PSP_OSMEM_CTL_SLOT_SIZE * PSP_OSMEM_NUM_SLOTS)
#define PSP_OSMEM_CONTAINER_SIZE                                               \
    (PSP_OSMEM_CONTAINER_SLOT_SIZE * PSP_OSMEM_NUM_SLOTS)

typedef uint64_t PSPOSMemAddr;

typedef struct PSPOSMemUART {
    uint8_t value;
} PSPOSMemUART;

/**
 * @brief This is a worse version of psp-x86.h with a bunch of hardcoded values
 *
 */
typedef struct PSPOSMemState {
    SysBusDevice parent_obj;

    /* MemoryRegion containing the MMIO registers */
    MemoryRegion psp_osmem_iomem;
    uint32_t psp_osmem_control[PSP_OSMEM_NUM_SLOTS];

    /* region in PSP address space */
    MemoryRegion psp_osmem_containers_iomem;

    // devices
    PSPOSMemUART uart;

    /* OS address space */
    // MemoryRegion psp_osmem_space;

    uint8_t psp_osmem_container_ram[PSP_OSMEM_CONTAINER_SLOT_SIZE];
} PSPOSMemState;
#endif
