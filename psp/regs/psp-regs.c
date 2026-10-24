/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"

#include "psp/psp-regs.h"

typedef struct PspRegEntry {
    uint64_t offset;
    uint64_t value;
    int32_t rw_slot;
} PspRegEntry;

typedef struct PspRamWindowTable {
    const PspRamWindow* windows;
    size_t count;
} PspRamWindowTable;

/* entries is sorted by offset. NULL with count 0 for an empty table. */
typedef struct PspRegTable {
    const PspRegEntry* entries;
    size_t count;
} PspRegTable;

/* Defines psp_regs_by_gen, flattened per generation, see gen_regs.py. */
#include "psp-regs.c.inc"

static const PspRegEntry* psp_regs_table_find(const PspRegTable* table,
                                              uint64_t offset)
{
    size_t lo = 0;
    size_t hi = table->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const PspRegEntry* e = &table->entries[mid];

        if (e->offset == offset) {
            return e;
        }
        if (e->offset < offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

bool psp_regs_find(PspGeneration gen, PspRegSpace space, uint64_t offset,
                   PspRegHit* hit)
{
    assert(gen < PSP_GENERATION_MAX);
    assert(space < PSP_REG_SPACE_MAX);

    const PspRegEntry* e = psp_regs_table_find(&psp_regs_by_gen[gen][space],
                                               offset);
    if (!e) {
        return false;
    }
    hit->value = e->value;
    hit->rw_slot = e->rw_slot;
    return true;
}

void psp_regs_rw_reset(PspGeneration gen, PspRegSpace space,
                       uint32_t slots[PSP_REGS_MAX_RW_SLOTS])
{
    const PspRegTable* table = &psp_regs_by_gen[gen][space];

    memset(slots, 0, sizeof(uint32_t) * PSP_REGS_MAX_RW_SLOTS);
    for (size_t i = 0; i < table->count; i++) {
        if (table->entries[i].rw_slot >= 0) {
            slots[table->entries[i].rw_slot] = table->entries[i].value;
        }
    }
}

bool psp_regs_lookup(PspGeneration gen, PspRegSpace space, uint64_t offset,
                     uint64_t* val)
{
    PspRegHit hit;

    if (!psp_regs_find(gen, space, offset, &hit)) {
        return false;
    }
    *val = hit.value;
    return true;
}

const PspRamWindow* psp_regs_ram_windows(PspGeneration gen, size_t* count)
{
    assert(gen < PSP_GENERATION_MAX);

    *count = psp_regs_ram_by_gen[gen].count;
    return psp_regs_ram_by_gen[gen].windows;
}
