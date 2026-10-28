/*
 * Copyright (C) 2024 Stefan Zabka <stefan@zabka.it>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PSP_X86_ACCESS_OBSERVER_H
#define PSP_X86_ACCESS_OBSERVER_H

#include "system/memory.h"
#include "hw/sysbus.h"

#include "psp/generations.h"
#include "psp/x86/psp-x86.h"

#define TYPE_PSP_X86_ACCESS_OBSERVER "amd_psp.x86.access-observer"
#define PSP_X86_ACCESS_OBSERVER(obj)                                           \
    OBJECT_CHECK(AccessObserverState, (obj), TYPE_PSP_X86_ACCESS_OBSERVER)

#define ACCESS_OBSERVER_MAX_EXCLUDES 4
#define ACCESS_OBSERVER_MAX_TOUCHED 256
#define ACCESS_OBSERVER_TOUCHED_SLOTS (2 * ACCESS_OBSERVER_MAX_TOUCHED)

#define ACCESS_OBSERVER_TOUCH_READ 0x1
#define ACCESS_OBSERVER_TOUCH_WRITE 0x2

/*
 * One 4 KiB x86 page touched inside the activated window (C ABI, 24 bytes,
 * natural alignment, little endian): page is the page-aligned x86 address,
 * pc the pc of the first access to the page of any kind, flags is
 * ACCESS_OBSERVER_TOUCH_READ | ACCESS_OBSERVER_TOUCH_WRITE, pad is zero.
 */
typedef struct {
    uint64_t page;
    uint64_t pc;
    uint8_t flags;
    uint8_t pad[7];
} AspfuzzTouchedPage;

typedef struct AccessObserverState {
    /* Base Class */
    SysBusDevice parent;
    /* Fields */
    hwaddr size;
    hwaddr base;

    /* Allow ranges inside the window */
    struct {
        hwaddr base;
        hwaddr size;
    } exclude[ACCESS_OBSERVER_MAX_EXCLUDES];
    unsigned int exclude_count;

    /*
     * Tracking Results: address and pc of the first illegal access of each
     * kind, valid only if the matching hit counter is non-zero
     */
    hwaddr observed_illegal_read;
    vaddr pc_illegal_read;
    uint64_t illegal_read_hits;
    hwaddr observed_illegal_write;
    vaddr pc_illegal_write;
    uint64_t illegal_write_hits;

    /*
     * Distinct pages touched inside the window, in first-touch order.
     * touched_index is an open-addressing hash (entry index + 1, 0 is empty)
     * and last_touched caches the previous hit; both only speed up lookups.
     */
    AspfuzzTouchedPage touched[ACCESS_OBSERVER_MAX_TOUCHED];
    uint16_t touched_index[ACCESS_OBSERVER_TOUCHED_SLOTS];
    uint32_t touched_count;
    uint64_t touched_overflow;
    int32_t last_touched;

    /* Intermediate Buffer*/
    MemoryRegion mr;

    /* Reference to the x86 memory*/
    PSPX86State* x86_memory;

    PspGeneration generation;

} AccessObserverState;

#endif // PSP_X86_ACCESS_OBSERVER_H
