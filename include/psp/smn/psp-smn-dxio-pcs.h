/*
 * AMD PSP Zen3 DXIO PCS state registers (SMN (idx << 20) + 0xF0000)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef AMD_PSP_SMN_DXIO_PCS_H
#define AMD_PSP_SMN_DXIO_PCS_H

#include "hw/sysbus.h"

#define TYPE_PSP_SMN_DXIO_PCS "amd_psp.smn.dxio-pcs"
#define PSP_SMN_DXIO_PCS(obj) \
    OBJECT_CHECK(PSPSmnDxioPcsState, (obj), TYPE_PSP_SMN_DXIO_PCS)

/* PCS instances 0x12e..0x133, SMN base of instance idx is idx << 20 */
#define PSP_SMN_DXIO_PCS_FIRST 0x12e
#define PSP_SMN_DXIO_PCS_COUNT 6
#define PSP_SMN_DXIO_PCS_STRIDE 0x100000
/* Offset of the register window inside an instance */
#define PSP_SMN_DXIO_PCS_OFFSET 0xf0000
#define PSP_SMN_DXIO_PCS_SIZE 0x100

typedef struct PSPSmnDxioPcsState PSPSmnDxioPcsState;

typedef struct PSPSmnDxioPcsInst {
    PSPSmnDxioPcsState *parent;     /* not migrated */
    unsigned index;                 /* not migrated */
    MemoryRegion iomem;
} PSPSmnDxioPcsInst;

struct PSPSmnDxioPcsState {
    /* <private> */
    SysBusDevice parent;
    /* <public> */
    PSPSmnDxioPcsInst inst[PSP_SMN_DXIO_PCS_COUNT];

    /* State: last value written to the command register of each instance */
    uint32_t state_hist[PSP_SMN_DXIO_PCS_COUNT];
};

#endif
