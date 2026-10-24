/*
 * AMD PSP emulation
 *
 * Copyright (C) 2020 Robert Buhren <robert@robertbuhren.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "qemu/osdep.h"

#include "psp/mmio/mailbox.h"

#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "trace.h"

#include "psp/psp.h"

static const char* index_to_name(int index)
{
    switch (index) {
    case 0:
        return "content";
    case 1:
        return "ptr_low";
    case 2:
        return "ptr_high";
    default:
        return "invalid";
    }
}
static uint64_t mailbox_read(void* opaque, hwaddr offset, unsigned int size)
{
    MailboxState* s = MAILBOX(opaque);
    hwaddr index = offset / 4;
    if (index >= 3) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: out-of-range mailbox read @ offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
    trace_psp_mailbox_read(index_to_name(index), s->values[index]);
    return extract32(s->values[index], (offset & 3) * 8, size * 8);
}

static MemTxResult mailbox_write(void* opaque, hwaddr offset, uint64_t value,
                                 unsigned int size, MemTxAttrs attrs)
{
    MailboxState* s = MAILBOX(opaque);
    bool armed;

    hwaddr index = offset / 4;
    if (index >= 3) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: out-of-range mailbox write @ offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return MEMTX_OK;
    }
    s->values[index] = deposit32(s->values[index], (offset & 3) * 8, size * 8,
                                 value);
    trace_psp_mailbox_write(index_to_name(index), s->values[index],
                            attrs.unspecified, attrs.secure, attrs.user,
                            attrs.debug, attrs.requester_id);
    /*
     * Doorbell: a word 0 write raises the source iff it is external
     * (attrs.unspecified) and the enable register arms it. Writes from
     * outside the guest carry MEMTXATTRS_UNSPECIFIED: qtest, and the
     * harness' physical memory writes (cpu_physical_memory_write,
     * address_space_write with MEMTXATTRS_UNSPECIFIED). The PSP core's TCG
     * accesses carry filled-in attributes, and the CCP maps guest memory
     * with its own explicit attributes (ccp_attrs in ccpv5.c), so neither
     * the firmware's replies nor a CCP copy onto the mailbox raise it,
     * whatever value they write.
     */
    if (index != 0 || s->doorbell_irq_id < 0) {
        return MEMTX_OK;
    }
    armed = s->int_enable & MAILBOX_INT_ENABLE_SECOS;
    if (armed && attrs.unspecified) {
        qemu_set_irq(s->doorbell_irq, 1);
    }
    return MEMTX_OK;
}

static uint64_t mailbox_ctrl_read(void* opaque, hwaddr offset,
                                  unsigned int size)
{
    MailboxState* s = MAILBOX(opaque);

    trace_psp_mailbox_ctrl_read(s->int_enable);
    return s->int_enable;
}

static void mailbox_ctrl_write(void* opaque, hwaddr offset, uint64_t value,
                               unsigned int size)
{
    MailboxState* s = MAILBOX(opaque);

    trace_psp_mailbox_ctrl_write(value);
    s->int_enable = value;
}

static const MemoryRegionOps mailbox_ctrl_ops = {
    .read = mailbox_ctrl_read,
    .write = mailbox_ctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const MemoryRegionOps mailbox_mem_ops = {
    .read = mailbox_read,
    .write_with_attrs = mailbox_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mailbox_reset_hold(Object* obj, ResetType type)
{
    MailboxState* s = MAILBOX(obj);

    memset(s->values, 0, sizeof(s->values));
    s->int_enable = 0;
}

static const VMStateDescription mailbox_vmstate = {
    .name = TYPE_MAILBOX,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(values, MailboxState, 3),
        VMSTATE_UINT32(int_enable, MailboxState),
        VMSTATE_END_OF_LIST()
    },
};

static void realize(DeviceState* dev, Error** errp)
{
    MailboxState* s = MAILBOX(dev);
    SysBusDevice* sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &mailbox_mem_ops, s,
                          TYPE_MAILBOX, 4 * 3);

    if (s->doorbell_irq_id < -1 || s->doorbell_irq_id >= 128) {
        error_setg(errp, "doorbell_irq_id must be -1 or below 128");
        return;
    }

    sysbus_init_mmio(sbd, &s->iomem);

    memory_region_init_io(&s->ctrl_iomem, OBJECT(dev), &mailbox_ctrl_ops, s,
                          TYPE_MAILBOX "-ctrl", 4);
    sysbus_init_mmio(sbd, &s->ctrl_iomem);

    sysbus_init_irq(sbd, &s->doorbell_irq);
}

static const Property mailbox_properties[] = {
    DEFINE_PROP_INT32("doorbell_irq_id", MailboxState, doorbell_irq_id,
                      MAILBOX_DOORBELL_GEN_DEFAULT),
};

static void class_init(ObjectClass* oc, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    ResettableClass* rc = RESETTABLE_CLASS(oc);

    dc->realize = realize;
    dc->vmsd = &mailbox_vmstate;
    rc->phases.hold = mailbox_reset_hold;
    device_class_set_props(dc, mailbox_properties);
}
static const TypeInfo psp_sts_info = {
    .name = TYPE_MAILBOX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .class_init = class_init,
    .instance_size = sizeof(MailboxState),

};

static void mailbox_register_types(void)
{
    type_register_static(&psp_sts_info);
}
type_init(mailbox_register_types);
