/*
 * AMD PSP Zen3 SMU message mailbox (SMN 0x3b10700)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef AMD_PSP_SMN_SMU_H
#define AMD_PSP_SMN_SMU_H

#include "hw/sysbus.h"

#define TYPE_PSP_SMN_SMU "amd_psp.smn.smu"
#define PSP_SMN_SMU(obj) OBJECT_CHECK(PSPSmnSmuState, (obj), TYPE_PSP_SMN_SMU)

/* SMN address of the register block */
#define PSP_SMN_SMU_BASE 0x3b10700
#define PSP_SMN_SMU_REGS 8      /* 32 bit registers */

typedef struct PSPSmnSmuState {
    /* <private> */
    SysBusDevice parent;
    /* <public> */
    MemoryRegion iomem;

    /* Properties */
    uint32_t df_pstates;    /* answer to QueryNumberOfDfPstates, 1..4 */
    uint32_t mem_clk_mhz;   /* answer to QueryMemFreqOfDfPstate */

    /* State */
    uint32_t regs[PSP_SMN_SMU_REGS];
} PSPSmnSmuState;

#endif
