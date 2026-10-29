/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * @see PspWindow for what this module does
 */

#include "qemu/osdep.h"

#include "psp/psp-window.h"

#include "qemu/bswap.h"
#include "qemu/rcu.h"

/* Forwarded accesses after which a window with RAM behind it becomes an alias */
#define PSP_WINDOW_PROMOTE_HITS 256

static GSList *psp_windows;

static bool window_target_has_ram(PspWindow *w)
{
    hwaddr addr = w->target;
    hwaddr end = w->target + w->size;

    RCU_READ_LOCK_GUARD();
    while (addr < end) {
        hwaddr xlat;
        hwaddr len = end - addr;
        MemoryRegion *mr = address_space_translate(w->as, addr, &xlat, &len,
                                                   false,
                                                   MEMTXATTRS_UNSPECIFIED);

        if (memory_region_is_ram(mr) || memory_region_is_romd(mr)) {
            return true;
        }
        if (!len) {
            break;
        }
        addr += len;
    }
    return false;
}

static void window_promote(PspWindow *w)
{
    if (w->direct || !window_target_has_ram(w)) {
        return;
    }
    memory_region_transaction_begin();
    memory_region_set_alias_offset(&w->alias, w->target);
    memory_region_set_enabled(&w->alias, true);
    memory_region_set_enabled(&w->soft, false);
    memory_region_transaction_commit();
    w->direct = true;
}

static void window_hit(PspWindow *w)
{
    if (w->hits < PSP_WINDOW_PROMOTE_HITS &&
        ++w->hits == PSP_WINDOW_PROMOTE_HITS) {
        window_promote(w);
    }
}

static MemTxResult window_read(void *opaque, hwaddr offset, uint64_t *data,
                               unsigned size, MemTxAttrs attrs)
{
    PspWindow *w = opaque;
    uint8_t buf[8] = {0};
    MemTxResult res;

    res = address_space_read(w->as, w->target + offset, attrs, buf, size);
    *data = ldn_le_p(buf, size);
    window_hit(w);
    return res;
}

static MemTxResult window_write(void *opaque, hwaddr offset, uint64_t data,
                                unsigned size, MemTxAttrs attrs)
{
    PspWindow *w = opaque;
    uint8_t buf[8];
    MemTxResult res;

    stn_le_p(buf, size, data);
    res = address_space_write(w->as, w->target + offset, attrs, buf, size);
    window_hit(w);
    return res;
}

static const MemoryRegionOps window_ops = {
    .read_with_attrs = window_read,
    .write_with_attrs = window_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .impl.unaligned = true,
};

void psp_window_init(PspWindow *w, Object *owner, const char *name,
                     MemoryRegion *parent, hwaddr base, MemoryRegion *target_mr,
                     AddressSpace *target_as, hwaddr target, hwaddr size)
{
    g_autofree char *soft_name = g_strdup_printf("%s-soft", name);

    w->as = target_as;
    w->target = target;
    w->base = base;
    w->size = size;
    w->hits = 0;
    w->direct = false;

    memory_region_init_alias(&w->alias, owner, name, target_mr, target, size);
    memory_region_set_enabled(&w->alias, false);
    memory_region_init_io(&w->soft, owner, &window_ops, w, soft_name, size);
    /*
     * An access to the window is not an access to the device that owns it, so
     * the target's own devices must not trip over it.
     */
    w->soft.disable_reentrancy_guard = true;

    memory_region_add_subregion_overlap(parent, base, &w->alias, 0);
    memory_region_add_subregion_overlap(parent, base, &w->soft, 0);
    psp_windows = g_slist_prepend(psp_windows, w);
}

void psp_window_set_target(PspWindow *w, hwaddr target)
{
    if (target == w->target) {
        return;
    }
    w->target = target;
    w->hits = 0;
    if (w->direct) {
        memory_region_transaction_begin();
        memory_region_set_enabled(&w->alias, false);
        memory_region_set_enabled(&w->soft, true);
        memory_region_transaction_commit();
        w->direct = false;
    }
}

void psp_window_begin_update(void)
{
    memory_region_transaction_begin();
}

void psp_window_end_update(void)
{
    memory_region_transaction_commit();
}

void psp_window_prepare_map(hwaddr addr, hwaddr len)
{
    if (!len) {
        return;
    }
    for (GSList *l = psp_windows; l; l = l->next) {
        PspWindow *w = l->data;

        if (addr - w->base < w->size ||
            (w->base > addr && w->base - addr < len)) {
            window_promote(w);
        }
    }
}
