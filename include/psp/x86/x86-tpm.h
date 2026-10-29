/*
 * AMD PSP Zen3 TPM TIS stand-in (x86 0xfed40000)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef X86_TPM_H
#define X86_TPM_H

#include "system/memory.h"
#include "hw/sysbus.h"

#define TYPE_PSP_X86_TPM "amd_psp.x86.tpm"
#define PSP_X86_TPM(obj) OBJECT_CHECK(PSPX86TpmState, (obj), TYPE_PSP_X86_TPM)

/* x86 address of locality 0; the localities are 0x1000 apart */
#define PSP_X86_TPM_BASE 0xfed40000UL

typedef struct PSPX86TpmState {
    /* <private> */
    SysBusDevice parent;
    /* <public> */
    MemoryRegion iomem;

    /* Properties */
    bool enabled;

    /* State */
    uint8_t access3;    /* ACCESS register of locality 3 */
} PSPX86TpmState;

#endif // X86_TPM_H
