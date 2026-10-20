/*
 * AMD PSP emulation
 *
 * Copyright (C) 2024 Stefan Zabka <stefan@zabka.it>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PSP_MMIO_MAILBOX_H
#define PSP_MMIO_MAILBOX_H

#include "hw/sysbus.h"

#define TYPE_MAILBOX "amd_psp.mailbox"
#define MAILBOX(obj) OBJECT_CHECK(MailboxState, (obj), TYPE_MAILBOX)

typedef struct MailboxState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t values[3];

    /* Device-side mailbox interrupt enable (0x0301069c): the SecOS mailbox
     * driver sets 0xff000000 here. Which bits matter is not known, so any
     * non-zero value arms the doorbell. */
    MemoryRegion ctrl_iomem;
    uint32_t int_enable;

    /* IC source raised on an x86-side write to word 0 (default 0x3c, the
     * command-mailbox id per psp-etl #658); -1 = none */
    int32_t doorbell_irq_id;
    qemu_irq doorbell_irq;
} MailboxState;

#endif
