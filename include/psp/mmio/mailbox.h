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

/* Word 0 bit 31: set by the firmware in its reply, clear in a host command */
#define MAILBOX_WORD0_REPLY BIT(31)

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
     * driver sets 0xff000000 here. Which bits matter is not known, so any
     * non-zero value arms the doorbell. */
    MemoryRegion ctrl_iomem;
    uint32_t int_enable;

    /*
     * IC source raised when the host writes a command to word 0. -1: none,
     * MAILBOX_DOORBELL_GEN_DEFAULT: resolved by the SoC from its generation.
     */
    int32_t doorbell_irq_id;
    /* Raise it on every word 0 write: no bit 31 or enable matching */
    bool doorbell_always;
    /*
     * Raise it on an armed word 0 write from outside the guest (attributes
     * unspecified) instead of keying on bit 31; PSP core writes never do
     */
    bool doorbell_from_x86_only;
    qemu_irq doorbell_irq;
} MailboxState;

#endif
