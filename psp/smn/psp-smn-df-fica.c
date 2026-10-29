/*
 * AMD PSP Zen3 data fabric indirect configuration access.
 *
 * Software reaches the data fabric configuration registers through an
 * address/data pair: FICAA at SMN 0x1d050 selects the register, FICAD at
 * SMN 0x1d080 reads or writes it. FICAA layout (inferred from the DF
 * documentation of other families, not from hardware): bit 0 access type,
 * bits [10:2] register dword, bits [13:11] function, bits [23:16] instance.
 * Only the function and the register are used to look a register up; the
 * instance and the remaining bits are ignored, so every instance sees the
 * same register file. Registers the guest never wrote read 0, except the
 * DRAM hole base (function 0, offset 0x104), which reads the value the
 * 'hole-base-default' property gives until the guest writes it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"

#include "psp/smn/psp-smn-df-fica.h"

#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "trace.h"

#define FICAA_KEY_MASK 0x3ffc   /* function [13:11] and register [10:2] */
#define FICAA_KEY_HOLE_BASE 0x104       /* function 0 */

static PSPSmnDfFicaEntry* df_fica_find(PSPSmnDfFicaState* s, uint32_t key)
{
    for (uint32_t i = 0; i < s->count; i++) {
        if (s->entries[i].key == key) {
            return &s->entries[i];
        }
    }
    return NULL;
}

static uint64_t df_ficaa_read(void* opaque, hwaddr offset, unsigned size)
{
    PSPSmnDfFicaState* s = opaque;

    return s->ficaa;
}

static void df_ficaa_write(void* opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    PSPSmnDfFicaState* s = opaque;

    s->ficaa = value;
}

static uint64_t df_ficad_read(void* opaque, hwaddr offset, unsigned size)
{
    PSPSmnDfFicaState* s = opaque;
    uint32_t key = s->ficaa & FICAA_KEY_MASK;
    PSPSmnDfFicaEntry* e = df_fica_find(s, key);
    uint32_t val = 0;

    if (e) {
        val = e->value;
    } else if (key == FICAA_KEY_HOLE_BASE) {
        val = s->hole_base_default;
    }

    trace_psp_smn_df_fica_read(s->ficaa, val);
    return val;
}

static void df_ficad_write(void* opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    PSPSmnDfFicaState* s = opaque;
    uint32_t key = s->ficaa & FICAA_KEY_MASK;
    PSPSmnDfFicaEntry* e = df_fica_find(s, key);

    trace_psp_smn_df_fica_write(s->ficaa, value);
    if (!e) {
        if (s->count == PSP_SMN_DF_FICA_ENTRIES) {
            qemu_log_mask(LOG_GUEST_ERROR, "psp-smn-df-fica: register store "
                          "full, dropping write to FICAA 0x%08x\n", s->ficaa);
            return;
        }
        e = &s->entries[s->count++];
        e->key = key;
    }
    e->value = value;
}

static const MemoryRegionOps df_ficaa_ops = {
    .read = df_ficaa_read,
    .write = df_ficaa_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static const MemoryRegionOps df_ficad_ops = {
    .read = df_ficad_read,
    .write = df_ficad_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void df_fica_realize(DeviceState* dev, Error** errp)
{
    PSPSmnDfFicaState* s = PSP_SMN_DF_FICA(dev);

    memory_region_init_io(&s->iomem[0], OBJECT(dev), &df_ficaa_ops, s,
                          "psp-smn-df-ficaa", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem[0]);
    memory_region_init_io(&s->iomem[1], OBJECT(dev), &df_ficad_ops, s,
                          "psp-smn-df-ficad", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem[1]);
}

static void df_fica_reset_hold(Object* obj, ResetType type)
{
    PSPSmnDfFicaState* s = PSP_SMN_DF_FICA(obj);

    s->ficaa = 0;
    s->count = 0;
    memset(s->entries, 0, sizeof(s->entries));
}

static const VMStateDescription vmstate_df_fica_entry = {
    .name = "psp-smn-df-fica-entry",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(key, PSPSmnDfFicaEntry),
        VMSTATE_UINT32(value, PSPSmnDfFicaEntry),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription df_fica_vmstate = {
    .name = "psp-smn-df-fica",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ficaa, PSPSmnDfFicaState),
        VMSTATE_UINT32(count, PSPSmnDfFicaState),
        VMSTATE_STRUCT_ARRAY(entries, PSPSmnDfFicaState,
                             PSP_SMN_DF_FICA_ENTRIES, 1,
                             vmstate_df_fica_entry, PSPSmnDfFicaEntry),
        VMSTATE_END_OF_LIST()
    }
};

static const Property df_fica_properties[] = {
    DEFINE_PROP_UINT32("hole-base-default", PSPSmnDfFicaState,
                       hole_base_default, 0xb0000001),
};

static void df_fica_class_init(ObjectClass* oc, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    ResettableClass* rc = RESETTABLE_CLASS(oc);

    dc->realize = df_fica_realize;
    dc->vmsd = &df_fica_vmstate;
    device_class_set_props(dc, df_fica_properties);
    rc->phases.hold = df_fica_reset_hold;
}

static const TypeInfo df_fica_info = {
    .name = TYPE_PSP_SMN_DF_FICA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PSPSmnDfFicaState),
    .class_init = df_fica_class_init,
};

static void df_fica_register_types(void)
{
    type_register_static(&df_fica_info);
}

type_init(df_fica_register_types)
