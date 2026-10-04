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

#include "system/memory.h"
#include "hw/sysbus.h"

#include "psp/generations.h"

#define FIQ_HANDLED_BITFIELD_OFFSET 0xb0
#define FIQ_ALL_PROCESSED_OFFSET 0xc0
#define FIQ_NEXT_OFFSET 0xc4


#define TYPE_PSP_IRQ "amd_psp.irq"
#define PSP_IRQ(obj) OBJECT_CHECK(PSPIrqState, (obj), TYPE_PSP_IRQ)

typedef struct PSPIrqState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    /* Bitfield */
    uint32_t ccp_queue_bitfield[4];

    struct {
        /* FIQ mark handled bitfield */
        uint32_t handled_bitfield[4];

        /* FIQ next */
        uint32_t next;

        /* FIQ still some left to process */
        uint32_t all_processed;
    } fiq;

    uint32_t irq_buffer[4];

    /* Identifier of this instance */
    char* ident;

    /* Which Generation is being emulated*/
    PspGeneration generation;

} PSPIrqState;

#endif
