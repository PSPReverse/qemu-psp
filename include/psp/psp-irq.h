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
#ifndef AMD_PSP_IRQ_H
#define AMD_PSP_IRQ_H

#include "qemu/osdep.h"

#include "exec/memory.h"
#include "hw/sysbus.h"

#include "psp/generations.h"

#define TYPE_PSP_IRQ "amd_psp.irq"
#define PSP_IRQ(obj) OBJECT_CHECK(PSPIrqState, (obj), TYPE_PSP_IRQ)

typedef struct PSPIrqState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    /* Bitfield */
    uint32_t bitfield[4];

    /* Identifier of this instance */
    char* ident;

    /* Which Generation is being emulated*/
    PspGeneration generation;

} PSPIrqState;

#endif
