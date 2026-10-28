/*
 * AMD PSP Zen3 UMC channel APB windows with a minimal PMU mailbox
 * (SMN 0x51800 + (channel << 20))
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef AMD_PSP_SMN_UMC_H
#define AMD_PSP_SMN_UMC_H

#include "hw/sysbus.h"

#define TYPE_PSP_SMN_UMC "amd_psp.smn.umc-apb"
#define PSP_SMN_UMC(obj) OBJECT_CHECK(PSPSmnUmcState, (obj), TYPE_PSP_SMN_UMC)

#define PSP_SMN_UMC_CHANNELS 8
/* SMN address of the first register of channel 0, channel stride 1 MiB */
#define PSP_SMN_UMC_BASE 0x51800
#define PSP_SMN_UMC_STRIDE 0x100000
#define PSP_SMN_UMC_REGS_SIZE 0x10      /* address, data, status */
/* Per channel register block at SMN 0x50000 + (channel << 20): plain storage */
#define PSP_SMN_UMC_CSR_BASE 0x50000
#define PSP_SMN_UMC_CSR_SIZE 0x300

/* APB ranges that are kept, one 32 bit value per APB address */
#define PSP_SMN_UMC_MBOX_BASE 0xd0000
#define PSP_SMN_UMC_MBOX_REGS 0x100
#define PSP_SMN_UMC_MSGBLK_BASE 0x54000
#define PSP_SMN_UMC_MSGBLK_REGS 0x400

typedef struct PSPSmnUmcState PSPSmnUmcState;

typedef struct PSPSmnUmcChannel {
    PSPSmnUmcState *parent;     /* not migrated */
    MemoryRegion iomem;
    MemoryRegion csr_iomem;
    GHashTable *logged;         /* APB addresses already reported, not migrated */

    uint32_t addr;              /* APB address register */
    uint32_t data;              /* APB data register */
    uint32_t csr[PSP_SMN_UMC_CSR_SIZE / 4]; /* BaseAddr, AddrMask, ... */
    bool acked;                 /* host acked the PMU message */
    uint32_t apb_mbox[PSP_SMN_UMC_MBOX_REGS];
    uint32_t apb_msg[PSP_SMN_UMC_MSGBLK_REGS];
} PSPSmnUmcChannel;

struct PSPSmnUmcState {
    /* <private> */
    SysBusDevice parent;
    /* <public> */

    /* Properties */
    uint32_t pmu_message;       /* code the PMU reports, default 7 = OK */

    /* State */
    PSPSmnUmcChannel chan[PSP_SMN_UMC_CHANNELS];
};

#endif
