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

#define MAILBOX_DOORBELL_GEN_DEFAULT (-2)

/*
 * Bits of the 0x0301069c enable that arm the host doorbell: SecOS writes
 * 0xff000000 there, the Zen3 off-chip BL writes 0xfc000 for its own use.
 */
#define MAILBOX_INT_ENABLE_SECOS 0xff000000

typedef struct MailboxState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t values[3];

    /* Device-side mailbox interrupt enable (0x0301069c): the SecOS mailbox
     * driver sets 0xff000000 here. The doorbell is armed while any bit of
     * MAILBOX_INT_ENABLE_SECOS is set. */
    MemoryRegion ctrl_iomem;
    uint32_t int_enable;

    /*
     * IC source raised when the host writes a command to word 0 (a write from
     * outside the guest while the enable register arms it). -1: none,
     * MAILBOX_DOORBELL_GEN_DEFAULT: resolved by the SoC from its generation.
     */
    int32_t doorbell_irq_id;
    qemu_irq doorbell_irq;
} MailboxState;

#endif
