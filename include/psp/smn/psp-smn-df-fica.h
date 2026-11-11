/*
 * AMD PSP Zen3 data fabric indirect configuration access (SMN 0x1d050/0x1d080)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef AMD_PSP_SMN_DF_FICA_H
#define AMD_PSP_SMN_DF_FICA_H

#include "hw/sysbus.h"

#define TYPE_PSP_SMN_DF_FICA "amd_psp.smn.df-fica"
#define PSP_SMN_DF_FICA(obj) \
    OBJECT_CHECK(PSPSmnDfFicaState, (obj), TYPE_PSP_SMN_DF_FICA)

/* SMN addresses of the address and data registers */
#define PSP_SMN_DF_FICAA 0x1d050
#define PSP_SMN_DF_FICAD 0x1d080
/* Direct SMN alias of the DF function 1 register 0x3f8 (flush control) */
#define PSP_SMN_DF_F1X3F8 0x1c7f8
/* Registers written by the guest that the store can hold */
#define PSP_SMN_DF_FICA_ENTRIES 256

typedef struct PSPSmnDfFicaEntry {
    uint32_t key;       /* function and register offset */
    uint32_t value;
} PSPSmnDfFicaEntry;

typedef struct PSPSmnDfFicaState {
    /* <private> */
    SysBusDevice parent;
    /* <public> */
    MemoryRegion iomem[3];      /* FICAA, FICAD, F1x3F8 */

    /* State */
    uint32_t ficaa;
    uint32_t count;
    uint32_t f1x3f8;
    /* Property */
    uint32_t hole_base_default;
    PSPSmnDfFicaEntry entries[PSP_SMN_DF_FICA_ENTRIES];
} PSPSmnDfFicaState;

#endif
