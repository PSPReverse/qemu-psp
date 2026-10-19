/*
 * Data-driven table of constant register values, see psp/regs/README.md.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PSP_REGS_H
#define PSP_REGS_H

#include "psp/generations.h"

typedef enum PspRegSpace {
    PSP_REG_SPACE_SMN_MISC,
    PSP_REG_SPACE_PSP_MISC,
    PSP_REG_SPACE_X86_MISC,
    PSP_REG_SPACE_MAX,
} PspRegSpace;

/**
 * @brief Look up a constant register value.
 *
 * Matches the exact @offset only. Searches the table of @gen for @space, then
 * the generic table for @space. On success stores the value in @val and
 * returns true. Otherwise @val is left untouched and false is returned.
 */
bool psp_regs_lookup(PspGeneration gen, PspRegSpace space, uint64_t offset,
                     uint64_t* val);

#endif
