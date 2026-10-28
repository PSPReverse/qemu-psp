/*
 * AMD PSP Zen3 UMC channel APB windows with a minimal PMU mailbox.
 *
 * Each of the 8 UMC channels has an indirect window to its APB space at SMN
 * 0x51800 + (channel << 20) (derived from the access sequence of the ABL DDR
 * training code, not from documentation):
 *   +0x00 APB address; bit 31 set turns the access into a write
 *   +0x04 APB data
 *   +0x08 status; bit 0 = 1 means done. The ABL polls it after every address
 *         write. Modelled as always 1 (inferred: no real latency is known).
 * Read of APB register A: write A, poll status, read data.
 * Write: write data, write A | 0x80000000, poll status.
 *
 * The per channel register block at SMN 0x50000..0x502ff (chip select base
 * and mask registers among others) is read/write-back storage, reset value 0.
 *
 * Behind the window sits the Synopsys PMU mailbox (inferred):
 *   0xd0004 message pending, bit 0; 0 = a message is available
 *   0xd0031 host ack, bit 0
 *   0xd0032 message code
 *   0xd0034 stream data word
 * The ABL polls 0xd0004 for 0, reads the code, acks with 0xd0031 = 0, polls
 * 0xd0004 for 1 and writes 0xd0031 = 1. The model reports the code of the
 * "pmu-message" property (0x07, training complete) and follows that handshake.
 * The ranges 0xd0000..0xd00ff and 0x54000..0x543ff (PMU message block) keep
 * what was written; everything else, in particular the PMU image and CSR
 * loads, is dropped on writes and reads 0.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"

#include "psp/smn/psp-smn-umc.h"

#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "trace.h"

#define REG_ADDR 0x00
#define REG_DATA 0x04
#define REG_STATUS 0x08

#define ADDR_WRITE_FLAG 0x80000000u
#define APB_MASK 0xfffffu
#define STATUS_DONE 1

#define APB_MBOX_PENDING 0xd0004
#define APB_MBOX_ACK 0xd0031
#define APB_MBOX_CODE 0xd0032

/* Report this many unknown APB addresses per channel, then go quiet */
#define LOG_LIMIT 64

static uint32_t *apb_slot(PSPSmnUmcChannel *c, uint32_t apb)
{
    if (apb - PSP_SMN_UMC_MBOX_BASE < PSP_SMN_UMC_MBOX_REGS) {
        return &c->apb_mbox[apb - PSP_SMN_UMC_MBOX_BASE];
    }
    if (apb - PSP_SMN_UMC_MSGBLK_BASE < PSP_SMN_UMC_MSGBLK_REGS) {
        return &c->apb_msg[apb - PSP_SMN_UMC_MSGBLK_BASE];
    }
    return NULL;
}

static void apb_log_unknown(PSPSmnUmcChannel *c, uint32_t apb, bool write,
                            uint32_t val)
{
    unsigned n = g_hash_table_size(c->logged);
    unsigned chan = c - c->parent->chan;

    if (n >= LOG_LIMIT ||
        !g_hash_table_add(c->logged, GUINT_TO_POINTER(apb))) {
        return;
    }
    qemu_log_mask(LOG_UNIMP, "psp-smn-umc: channel %u: unknown APB %s 0x%05x "
                  "value 0x%x, ignored (reported once%s)\n", chan,
                  write ? "write" : "read", apb, val,
                  n + 1 == LOG_LIMIT ? ", last report for this channel" : "");
}

static uint32_t apb_read(PSPSmnUmcChannel *c, uint32_t apb)
{
    uint32_t *slot = apb_slot(c, apb);

    switch (apb) {
    case APB_MBOX_PENDING:
        return c->acked ? 1 : 0;
    case APB_MBOX_CODE:
        return c->parent->pmu_message & 0xffff;
    }
    if (!slot) {
        apb_log_unknown(c, apb, false, 0);
        return 0;
    }
    return *slot;
}

static void apb_write(PSPSmnUmcChannel *c, uint32_t apb, uint32_t val)
{
    uint32_t *slot = apb_slot(c, apb);

    if (apb == APB_MBOX_ACK) {
        c->acked = !(val & 1);
    }
    if (!slot) {
        /* Image and CSR loads land here in bulk, only report the first few */
        apb_log_unknown(c, apb, true, val);
        return;
    }
    *slot = val;
}

static uint64_t psp_umc_read(void *opaque, hwaddr offset, unsigned size)
{
    PSPSmnUmcChannel *c = opaque;
    uint32_t val = 0;

    switch (offset) {
    case REG_ADDR:
        val = c->addr;
        break;
    case REG_DATA:
        val = c->data;
        break;
    case REG_STATUS:
        val = STATUS_DONE;
        break;
    }
    trace_psp_smn_umc_read(c - c->parent->chan, offset, val);
    return val;
}

static void psp_umc_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    PSPSmnUmcChannel *c = opaque;
    unsigned chan = c - c->parent->chan;

    trace_psp_smn_umc_write(chan, offset, value);
    switch (offset) {
    case REG_ADDR:
        c->addr = value;
        if (value & ADDR_WRITE_FLAG) {
            apb_write(c, value & APB_MASK, c->data);
            trace_psp_smn_umc_apb(chan, true, value & APB_MASK, c->data);
        } else {
            c->data = apb_read(c, value & APB_MASK);
            trace_psp_smn_umc_apb(chan, false, value & APB_MASK, c->data);
        }
        break;
    case REG_DATA:
        c->data = value;
        break;
    }
}

static const MemoryRegionOps psp_umc_ops = {
    .read = psp_umc_read,
    .write = psp_umc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static uint64_t psp_umc_csr_read(void *opaque, hwaddr offset, unsigned size)
{
    PSPSmnUmcChannel *c = opaque;
    uint32_t val = (c->csr[offset / 4] >> ((offset & 3) * 8)) &
                   MAKE_64BIT_MASK(0, size * 8);

    trace_psp_smn_umc_csr_read(c - c->parent->chan, offset, val);
    return val;
}

static void psp_umc_csr_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    PSPSmnUmcChannel *c = opaque;
    uint32_t *slot = &c->csr[offset / 4];
    uint32_t shift = (offset & 3) * 8;
    uint32_t mask = (uint32_t)MAKE_64BIT_MASK(0, size * 8) << shift;

    *slot = (*slot & ~mask) | (((uint32_t)value << shift) & mask);
    trace_psp_smn_umc_csr_write(c - c->parent->chan, offset, value);
}

static const MemoryRegionOps psp_umc_csr_ops = {
    .read = psp_umc_csr_read,
    .write = psp_umc_csr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void psp_umc_realize(DeviceState *dev, Error **errp)
{
    PSPSmnUmcState *s = PSP_SMN_UMC(dev);

    for (unsigned i = 0; i < PSP_SMN_UMC_CHANNELS; i++) {
        PSPSmnUmcChannel *c = &s->chan[i];
        g_autofree char *name = g_strdup_printf("psp-smn-umc%u", i);

        c->parent = s;
        c->logged = g_hash_table_new(g_direct_hash, g_direct_equal);
        memory_region_init_io(&c->iomem, OBJECT(dev), &psp_umc_ops, c, name,
                              PSP_SMN_UMC_REGS_SIZE);
        sysbus_init_mmio(SYS_BUS_DEVICE(dev), &c->iomem);
    }
    /* MMIO regions 8..15: the register blocks */
    for (unsigned i = 0; i < PSP_SMN_UMC_CHANNELS; i++) {
        PSPSmnUmcChannel *c = &s->chan[i];
        g_autofree char *name = g_strdup_printf("psp-smn-umc-csr%u", i);

        memory_region_init_io(&c->csr_iomem, OBJECT(dev), &psp_umc_csr_ops, c,
                              name, PSP_SMN_UMC_CSR_SIZE);
        sysbus_init_mmio(SYS_BUS_DEVICE(dev), &c->csr_iomem);
    }
}

static void psp_umc_reset_hold(Object *obj, ResetType type)
{
    PSPSmnUmcState *s = PSP_SMN_UMC(obj);

    for (unsigned i = 0; i < PSP_SMN_UMC_CHANNELS; i++) {
        PSPSmnUmcChannel *c = &s->chan[i];

        c->addr = 0;
        c->data = 0;
        c->acked = false;
        memset(c->csr, 0, sizeof(c->csr));
        memset(c->apb_mbox, 0, sizeof(c->apb_mbox));
        memset(c->apb_msg, 0, sizeof(c->apb_msg));
        g_hash_table_remove_all(c->logged);
    }
}

static const VMStateDescription psp_umc_chan_vmstate = {
    .name = "psp-smn-umc-channel",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(addr, PSPSmnUmcChannel),
        VMSTATE_UINT32(data, PSPSmnUmcChannel),
        VMSTATE_BOOL(acked, PSPSmnUmcChannel),
        VMSTATE_UINT32_ARRAY(csr, PSPSmnUmcChannel, PSP_SMN_UMC_CSR_SIZE / 4),
        VMSTATE_UINT32_ARRAY(apb_mbox, PSPSmnUmcChannel,
                             PSP_SMN_UMC_MBOX_REGS),
        VMSTATE_UINT32_ARRAY(apb_msg, PSPSmnUmcChannel,
                             PSP_SMN_UMC_MSGBLK_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription psp_umc_vmstate = {
    .name = "psp-smn-umc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(chan, PSPSmnUmcState, PSP_SMN_UMC_CHANNELS, 1,
                             psp_umc_chan_vmstate, PSPSmnUmcChannel),
        VMSTATE_END_OF_LIST()
    }
};

static const Property psp_umc_properties[] = {
    DEFINE_PROP_UINT32("pmu-message", PSPSmnUmcState, pmu_message, 7),
};

static void psp_umc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = psp_umc_realize;
    dc->vmsd = &psp_umc_vmstate;
    rc->phases.hold = psp_umc_reset_hold;
    device_class_set_props(dc, psp_umc_properties);
}

static const TypeInfo psp_umc_info = {
    .name = TYPE_PSP_SMN_UMC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PSPSmnUmcState),
    .class_init = psp_umc_class_init,
};

static void psp_umc_register_types(void)
{
    type_register_static(&psp_umc_info);
}

type_init(psp_umc_register_types)
