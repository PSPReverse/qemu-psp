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
#ifndef AMD_PSP_TIMER_H
#define AMD_PSP_TIMER_H

#include "system/memory.h"
#include "hw/sysbus.h"
#include "qemu/timer.h"

#define TYPE_PSP_TIMER "amd_psp.timer"
#define PSP_TIMER(obj) OBJECT_CHECK(PSPTimerState, (obj), TYPE_PSP_TIMER)

/* PSP Timer iomem size */
#define PSP_TIMER_SIZE 0x24

typedef struct PSPTimerState {
    SysBusDevice parent_obj;

    /* MemoryRegion containing the MMIO registers */
    MemoryRegion psp_timer_iomem;

    uint32_t psp_timer_control;
    uint32_t psp_timer_count;

    /* Reload value written at +0x10 (100 MHz counts) */
    uint32_t psp_timer_period;

    /* Read-back for the registers at +0x4/+0x8/+0xc (meaning not modelled) */
    uint32_t psp_timer_misc[4];

    /*
     * Placeholder read-back for the registers at +0x14/+0x18/+0x1c. Accessed
     * by the Zen2 off-chip BL; meaning is inferred, not modelled.
     */
    uint32_t psp_timer_placeholder[3];

    /*
     * Opt-in: non-zero derives the counter from QEMU_CLOCK_VIRTUAL at this
     * rate (Hz) instead of counting +1 per read; 0 = off.
     */
    uint32_t counter_hz;
    /* Virtual time at which psp_timer_count was last written (derived mode) */
    int64_t counter_anchor_ns;
    /*
     * Extra virtual time (ns) added to the derived counter by
     * aspfuzz_timer_advance(); zeroed on reset
     */
    uint64_t counter_offset_ns;

    /* Opt-in expiry interrupt: IC source raised every period while enabled;
     * -1 = off (keeps snapshots free of host-clock timers). */
    int32_t expiry_irq_id;
    qemu_irq expiry_irq;
    QEMUTimer expiry_timer;
    /*
     * Time left on expiry_timer when the state was saved, -1 if not pending.
     * Saved as a delta: the absolute deadline is in the old clock's frame.
     */
    int64_t expiry_remaining_ns;

} PSPTimerState;

/* Fuzzer hook, see psp-timer.c */
int32_t aspfuzz_timer_advance(uint64_t ns);

/* 64-bit counter for PSP-misc 0x032001f0/f4, 0 unless counter_hz != 0 */
uint64_t psp_timer_system_ticks(void);
#endif
