/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"

#include "psp/psp-regs.h"

typedef struct PspRegEntry {
    uint64_t offset;
    uint64_t value;
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

static bool psp_regs_table_lookup(const PspRegTable* table, uint64_t offset,
                                  uint64_t* val)
{
    size_t lo = 0;
    size_t hi = table->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const PspRegEntry* e = &table->entries[mid];

        if (e->offset == offset) {
            *val = e->value;
            return true;
        }
        if (e->offset < offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

bool psp_regs_lookup(PspGeneration gen, PspRegSpace space, uint64_t offset,
                     uint64_t* val)
{
    assert(gen < PSP_GENERATION_MAX);
    assert(space < PSP_REG_SPACE_MAX);

    return psp_regs_table_lookup(&psp_regs_by_gen[gen][space], offset, val);
}

const PspRamWindow* psp_regs_ram_windows(PspGeneration gen, size_t* count)
{
    assert(gen < PSP_GENERATION_MAX);

    *count = psp_regs_ram_by_gen[gen].count;
    return psp_regs_ram_by_gen[gen].windows;
}
