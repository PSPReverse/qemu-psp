/*
 * AMD PSP Zen3 SEV key-slot registers of the CCX SMN windows
 *
 * The Zen3 SEV applet programs the key of an ASID into every CCX through
 * the window at SMN 0x50000 | ccx << 20: it writes 0x80000000 | asid << 2 to
 * +0xa00, then four dwords to +0xa04, writes +0xa00 again and reads four
 * dwords back from +0xa04 and compares them. +0xa04 is therefore a data port:
 * every write to +0xa00 selects a slot and rewinds it, each access to +0xa04
 * moves to the next of the slot's four words. +0x11ac is plain storage. The
 * applet also reads bit 31 of +0x100 to see whether the CCX exists; that
 * address lies in the plain storage block of the UMC channels
 * (psp-smn-umc.c) and is left to it. The meaning is inferred from the
 * applet's use, not from documentation.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"

#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "trace.h"

#include "psp/smn/psp-smn-sevkey.h"


typedef PSPSmnSevkeyPort SevkeyOpaque;

static uint32_t *slot_of(PSPSmnSevkeyState *s, unsigned ccx)
{
    unsigned slot = (s->index[ccx] >> 2) % PSP_SMN_SEVKEY_SLOTS;

    return &s->slots[(ccx * PSP_SMN_SEVKEY_SLOTS + slot) * 4];
}

static uint64_t key_read(void *opaque, hwaddr offset, unsigned size)
{
    SevkeyOpaque *o = opaque;
    PSPSmnSevkeyState *s = o->s;
    uint32_t val;

    if (offset == 0) {
        return s->index[o->ccx];
    }
    val = slot_of(s, o->ccx)[s->pointer[o->ccx]++ & 3];
    trace_psp_smn_sevkey_read(o->ccx, s->index[o->ccx], val);
    return val;
}

static void key_write(void *opaque, hwaddr offset, uint64_t val,
                      unsigned size)
{
    SevkeyOpaque *o = opaque;
    PSPSmnSevkeyState *s = o->s;

    if (offset == 0) {
        s->index[o->ccx] = val;
        s->pointer[o->ccx] = 0;
        return;
    }
    trace_psp_smn_sevkey_write(o->ccx, s->index[o->ccx], val);
    slot_of(s, o->ccx)[s->pointer[o->ccx]++ & 3] = val;
}

static uint64_t scratch_read(void *opaque, hwaddr offset, unsigned size)
{
    SevkeyOpaque *o = opaque;

    return o->s->scratch[o->ccx];
}

static void scratch_write(void *opaque, hwaddr offset, uint64_t val,
                          unsigned size)
{
    SevkeyOpaque *o = opaque;

    o->s->scratch[o->ccx] = val;
}

#define OPS(name) \
    static const MemoryRegionOps name##_ops = { \
        .read = name##_read, \
        .write = name##_write, \
        .endianness = DEVICE_LITTLE_ENDIAN, \
        .valid.min_access_size = 4, \
        .valid.max_access_size = 4, \
        .impl.min_access_size = 4, \
        .impl.max_access_size = 4, \
    }
OPS(key);
OPS(scratch);

static void sevkey_realize(DeviceState *dev, Error **errp)
{
    PSPSmnSevkeyState *s = PSP_SMN_SEVKEY(dev);

    for (unsigned i = 0; i < PSP_SMN_SEVKEY_CCXS; i++) {
        SevkeyOpaque *o = &s->ports[i];

        o->s = s;
        o->ccx = i;
        memory_region_init_io(&s->iomem[i][0], OBJECT(dev), &key_ops, o,
                              "psp-smn-sevkey-port", 8);
        memory_region_init_io(&s->iomem[i][1], OBJECT(dev), &scratch_ops, o,
                              "psp-smn-sevkey-scratch", 4);
        for (unsigned r = 0; r < PSP_SMN_SEVKEY_REGIONS; r++) {
            sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem[i][r]);
        }
    }
}

static void sevkey_reset_hold(Object *obj, ResetType type)
{
    PSPSmnSevkeyState *s = PSP_SMN_SEVKEY(obj);

    memset(s->index, 0, sizeof(s->index));
    memset(s->pointer, 0, sizeof(s->pointer));
    memset(s->scratch, 0, sizeof(s->scratch));
    memset(s->slots, 0, sizeof(s->slots));
}

static const VMStateDescription sevkey_vmstate = {
    .name = TYPE_PSP_SMN_SEVKEY,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(index, PSPSmnSevkeyState, PSP_SMN_SEVKEY_CCXS),
        VMSTATE_UINT32_ARRAY(pointer, PSPSmnSevkeyState, PSP_SMN_SEVKEY_CCXS),
        VMSTATE_UINT32_ARRAY(scratch, PSPSmnSevkeyState, PSP_SMN_SEVKEY_CCXS),
        VMSTATE_UINT32_ARRAY(slots, PSPSmnSevkeyState,
                             PSP_SMN_SEVKEY_CCXS * PSP_SMN_SEVKEY_SLOTS * 4),
        VMSTATE_END_OF_LIST()
    }
};

static void sevkey_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = sevkey_realize;
    dc->vmsd = &sevkey_vmstate;
    rc->phases.hold = sevkey_reset_hold;
}

static const TypeInfo sevkey_info = {
    .name = TYPE_PSP_SMN_SEVKEY,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PSPSmnSevkeyState),
    .class_init = sevkey_class_init,
};

static void sevkey_register_types(void)
{
    type_register_static(&sevkey_info);
}

type_init(sevkey_register_types);
