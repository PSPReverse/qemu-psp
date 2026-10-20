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

    /* Per-group enable mask (+0x0..0xc), one word per group of 32 sources */
    uint32_t enable_mask[4];

    /*
     * Pending sources, one word per group of 32 sources (source = group * 32
     * + bit).
     *
     * Input contract: each source is one GPIO input, driven with level
     * semantics. Raising the input sets its pending bit, lowering it clears
     * the pending bit. The firmware ACKs through FIQ_HANDLED, which clears
     * pending bits whether or not the input is still high. A pulse that is
     * raised and lowered before the core runs is lost, so a source that needs
     * an edge must hold its input high until it is serviced.
     */
    uint32_t irq_buffer[4];

    /*
     * Core IRQ and FIQ lines. Both are asserted while an enabled source is
     * pending.
     */
    qemu_irq parent_irq;
    qemu_irq parent_fiq;

    /* Debug knob, not device behaviour: seeds group-0 pending bits at reset */
    uint32_t force_group0;

    /* Identifier of this instance */
    char* ident;

    /* Which Generation is being emulated*/
    PspGeneration generation;

} PSPIrqState;

#endif
