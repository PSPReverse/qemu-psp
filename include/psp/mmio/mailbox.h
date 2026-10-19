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

    /* Debug: IC source raised on a write to word 0; -1 = none */
    int32_t doorbell_irq_id;
    qemu_irq doorbell_irq;
} MailboxState;

#endif
