/*
 * AMD PSP Zen3 MP5 stand-in (SMN 0x3030008c)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef AMD_PSP_SMN_MP5_H
#define AMD_PSP_SMN_MP5_H

#include "hw/sysbus.h"
#include "qemu/timer.h"

#define TYPE_PSP_SMN_MP5 "amd_psp.smn.mp5"
#define PSP_SMN_MP5(obj) OBJECT_CHECK(PSPSmnMp5State, (obj), TYPE_PSP_SMN_MP5)

/* SMN address of the MP5 control register (die 0) */
#define PSP_SMN_MP5_BASE 0x3030008c
/* PSP address of the mailbox word the stand-in answers through */
#define PSP_MP5_MAILBOX 0x0301071c
#define PSP_MP5_MAILBOX_VALUE 0x001c0000
/* Interrupt controller source the stand-in raises (group 0, bit 16) */
#define PSP_MP5_IRQ_ID 0x10

typedef struct PSPSmnMp5State {
    /* <private> */
    SysBusDevice parent;
    /* <public> */
    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer* timer;

    /* Properties */
    bool enabled;
    uint32_t delay_us;

    /* State */
    uint32_t ctrl;
    bool loading;       /* 2 was written, the closing 0 starts the MP5 */
} PSPSmnMp5State;

#endif
