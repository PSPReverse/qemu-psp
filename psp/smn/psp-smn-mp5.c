/*
 * AMD PSP Zen3 MP5 stand-in.
 *
 * The Zen3 off-chip BL waits without a bound for the MP5 (the power
 * management microcontroller) to come up after loading its firmware. The
 * sequence, inferred from the BL's accesses and not from documentation:
 * write 2 to SMN 0x3030008c, load the MP5 firmware, set bit 1 of
 * 0x303000e0, write 0 to 0x3030008c. Then the MP5 sends a command through
 * the PSP mailbox word 0x0301071c (command in bits 23:16) and raises
 * interrupt controller source 0x10; the BL's handler answers by writing the
 * word back with bit 31 set.
 *
 * This stand-in does that a short virtual-time delay after the closing
 * write of 0, once, for die 0 only. 0x3030008c reads/writes back.
 *
 * The interrupt controller latches a source when its line goes high and
 * drops it on the firmware's ACK write (a low line clears it as well), so
 * the line is raised once and left high: pending until acknowledged.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"

#include "psp/smn/psp-smn-mp5.h"

#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "trace.h"

#define CTRL_LOAD 2

static void psp_mp5_fire(void* opaque)
{
    PSPSmnMp5State* s = opaque;

    trace_psp_smn_mp5_fire();
    address_space_stl_le(&address_space_memory, PSP_MP5_MAILBOX,
                         PSP_MP5_MAILBOX_VALUE, MEMTXATTRS_UNSPECIFIED, NULL);
    qemu_set_irq(s->irq, 1);
}

static uint64_t psp_mp5_read(void* opaque, hwaddr offset, unsigned size)
{
    PSPSmnMp5State* s = opaque;

    trace_psp_smn_mp5_read(s->ctrl);
    return s->ctrl;
}

static void psp_mp5_write(void* opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    PSPSmnMp5State* s = opaque;

    trace_psp_smn_mp5_write(value);
    s->ctrl = value;
    if (value == CTRL_LOAD) {
        s->loading = true;
    } else if (value == 0 && s->loading) {
        s->loading = false;
        if (s->enabled) {
            trace_psp_smn_mp5_trigger(s->delay_us);
            timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                (int64_t)s->delay_us * SCALE_US);
        }
    } else {
        s->loading = false;
    }
}

static const MemoryRegionOps psp_mp5_ops = {
    .read = psp_mp5_read,
    .write = psp_mp5_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void psp_mp5_realize(DeviceState* dev, Error** errp)
{
    PSPSmnMp5State* s = PSP_SMN_MP5(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &psp_mp5_ops, s,
                          "psp-smn-mp5", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, psp_mp5_fire, s);
}

static void psp_mp5_unrealize(DeviceState* dev)
{
    PSPSmnMp5State* s = PSP_SMN_MP5(dev);

    timer_free(s->timer);
}

static void psp_mp5_reset_hold(Object* obj, ResetType type)
{
    PSPSmnMp5State* s = PSP_SMN_MP5(obj);

    timer_del(s->timer);
    s->ctrl = 0;
    s->loading = false;
}

static const VMStateDescription psp_mp5_vmstate = {
    .name = "psp-smn-mp5",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, PSPSmnMp5State),
        VMSTATE_BOOL(loading, PSPSmnMp5State),
        VMSTATE_TIMER_PTR(timer, PSPSmnMp5State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property psp_mp5_properties[] = {
    DEFINE_PROP_BOOL("enabled", PSPSmnMp5State, enabled, true),
    DEFINE_PROP_UINT32("delay-us", PSPSmnMp5State, delay_us, 1000),
};

static void psp_mp5_class_init(ObjectClass* oc, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    ResettableClass* rc = RESETTABLE_CLASS(oc);

    dc->realize = psp_mp5_realize;
    dc->unrealize = psp_mp5_unrealize;
    dc->vmsd = &psp_mp5_vmstate;
    rc->phases.hold = psp_mp5_reset_hold;
    device_class_set_props(dc, psp_mp5_properties);
}

static const TypeInfo psp_mp5_info = {
    .name = TYPE_PSP_SMN_MP5,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PSPSmnMp5State),
    .class_init = psp_mp5_class_init,
};

static void psp_mp5_register_types(void)
{
    type_register_static(&psp_mp5_info);
}

type_init(psp_mp5_register_types)
