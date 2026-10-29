/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A window of the PSP address space that shows a movable part of another
 * address space (the SMN slots, the x86 mapper slots).
 */
#ifndef PSP_WINDOW_H
#define PSP_WINDOW_H

#include "system/memory.h"

/*
 * The guest moves these windows very often, mostly to touch a few registers.
 * Moving an alias costs a rebuild of the memory map, so a window starts out
 * as an I/O region that forwards every access to the target address space at
 * its current offset, which makes moving it a plain store. Only a window that
 * is used a lot while it stays put, or that is about to be DMA mapped, is
 * switched to an alias, which RAM needs to be accessed at full speed.
 */
typedef struct PspWindow {
    /* Alias onto the target, enabled while @direct */
    MemoryRegion alias;
    /* Forwards accesses to the target, enabled while not @direct */
    MemoryRegion soft;
    AddressSpace *as;
    /* Address in @as the window currently shows */
    hwaddr target;
    hwaddr base;
    hwaddr size;
    /* Accesses forwarded since @target changed, stops counting at the limit */
    uint32_t hits;
    bool direct;
} PspWindow;

/**
 * psp_window_init: map a window into @parent at @base
 * @target_mr: root of @target_as, the region the alias is made on
 * @target: initial offset into the target
 */
void psp_window_init(PspWindow *w, Object *owner, const char *name,
                     MemoryRegion *parent, hwaddr base, MemoryRegion *target_mr,
                     AddressSpace *target_as, hwaddr target, hwaddr size);

/* Point the window at @target in its address space */
void psp_window_set_target(PspWindow *w, hwaddr target);

/* Point many windows in one memory map update */
void psp_window_begin_update(void);
void psp_window_end_update(void);

/*
 * Make every window overlapping [@addr, @addr + @len) of the PSP address
 * space an alias if its target holds RAM, so that address_space_map() gets
 * the RAM and not a bounce buffer.
 */
void psp_window_prepare_map(hwaddr addr, hwaddr len);

#endif /* PSP_WINDOW_H */
