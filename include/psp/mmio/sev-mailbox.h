/*
 * AMD PSP Zen3 SEV firmware mailbox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PSP_MMIO_SEV_MAILBOX_H
#define PSP_MMIO_SEV_MAILBOX_H

#include "hw/sysbus.h"

#define TYPE_SEV_MAILBOX "amd_psp.sev-mailbox"
#define SEV_MAILBOX(obj) OBJECT_CHECK(SevMailboxState, (obj), \
                                      TYPE_SEV_MAILBOX)

/* PSP addresses of the command/reply word and the x86 buffer address */
#define PSP_SEV_MAILBOX_CMD 0x03010980
#define PSP_SEV_MAILBOX_ADDR_LO 0x030109e0
#define PSP_SEV_MAILBOX_ADDR_HI 0x030109e4

#define SEV_MAILBOX_WORDS 3
/* The Zen3 BL takes the SEV command on IC source 0x40 (group 2, bit 0) */
#define SEV_MAILBOX_DOORBELL_DEFAULT 0x40

typedef struct SevMailboxState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem[SEV_MAILBOX_WORDS];
    uint32_t values[SEV_MAILBOX_WORDS];

    /*
     * IC source raised when the host writes a command (bit 31 clear) to word
     * 0 from outside the guest. -1: none.
     */
    int32_t doorbell_irq_id;
    qemu_irq doorbell_irq;
} SevMailboxState;

#endif
