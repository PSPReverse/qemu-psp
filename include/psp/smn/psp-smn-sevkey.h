/*
 * AMD PSP Zen3 SEV key-slot registers of the CCX SMN windows
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef AMD_PSP_SMN_SEVKEY_H
#define AMD_PSP_SMN_SEVKEY_H

#include "hw/core/sysbus.h"

#define TYPE_PSP_SMN_SEVKEY "amd_psp.smn.sevkey"
#define PSP_SMN_SEVKEY(obj) \
    OBJECT_CHECK(PSPSmnSevkeyState, (obj), TYPE_PSP_SMN_SEVKEY)

/* CCX n has its registers in the window at 0x50000 | n << 20 */
#define PSP_SMN_SEVKEY_CCX_BASE(n) (0x50000 + ((n) << 20))
#define PSP_SMN_SEVKEY_CCXS 8
#define PSP_SMN_SEVKEY_SLOTS 1024       /* index bits 2..11 */
#define PSP_SMN_SEVKEY_REGIONS 2        /* key port, scratch */

struct PSPSmnSevkeyState;

typedef struct PSPSmnSevkeyPort {
    struct PSPSmnSevkeyState *s;
    unsigned ccx;
} PSPSmnSevkeyPort;

typedef struct PSPSmnSevkeyState {
    /* <private> */
    SysBusDevice parent;
    /* <public> */
    MemoryRegion iomem[PSP_SMN_SEVKEY_CCXS][PSP_SMN_SEVKEY_REGIONS];

    /* State */
    uint32_t index[PSP_SMN_SEVKEY_CCXS];    /* last value written to 0xa00 */
    uint32_t pointer[PSP_SMN_SEVKEY_CCXS];  /* next data word of the slot */
    uint32_t scratch[PSP_SMN_SEVKEY_CCXS];  /* plain storage at 0x11ac */
    /* [ccx][slot][word], flattened for the vmstate */
    uint32_t slots[PSP_SMN_SEVKEY_CCXS * PSP_SMN_SEVKEY_SLOTS * 4];

    /* Not migrated */
    PSPSmnSevkeyPort ports[PSP_SMN_SEVKEY_CCXS];
} PSPSmnSevkeyState;

#endif
