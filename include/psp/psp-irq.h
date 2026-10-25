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

/* Interrupt sources: 4 groups of 32 */
#define PSP_IRQ_NUM_SOURCES (4 * 32)

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

    /*
     * Pending interrupt bitfield: 4 priority groups x 32 device sources.
     * Sources are raised via the device's GPIO input lines, reported to the
     * firmware via FIQ_NEXT/FIQ_ALL_PROCESSED and cleared (ACKed) via
     * FIQ_HANDLED. The core line is asserted while any enabled bit is set.
     */
    uint32_t irq_buffer[4];

    /* Output line to the PSP core, asserted while an enabled irq_buffer bit is set. */
    qemu_irq parent_irq;
    qemu_irq parent_fiq;

    /* Debug: group-0/1/3 sources forced pending at reset */
    uint32_t force_group0;
    uint32_t force_group1;
    uint32_t force_group3;

    /* Identifier of this instance */
    char* ident;

    /* Which Generation is being emulated*/
    PspGeneration generation;

} PSPIrqState;

#endif
