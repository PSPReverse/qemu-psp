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

#include "psp/mmio/psp-sts.h"

#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "trace.h"

static char const* bool_to_str(bool b)
{
    if (b) {
        return "true";
    } else {
        return "false";
    }
}

static uint64_t psp_sts_read(void* opaque, hwaddr offset, unsigned int size)
{
    PSPStsState* s = PSP_STS(opaque);
    trace_psp_sts_read(s->psp_sts_val, bool_to_str(s->in_x86_space));
    return s->psp_sts_val;
}

static void psp_sts_write(void* opaque, hwaddr offset, uint64_t value,
                          unsigned int size)
{
    PSPStsState* s = PSP_STS(opaque);
    s->psp_sts_val = value;
    trace_psp_sts_write(value, bool_to_str(s->in_x86_space));
}

static const MemoryRegionOps sts_mem_ops = {
    .read = psp_sts_read,
    .write = psp_sts_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void psp_sts_init(Object* obj)
{
    PSPStsState* s = PSP_STS(obj);
    SysBusDevice* sbd = SYS_BUS_DEVICE(obj);

    s->psp_sts_val = 0;
    s->in_x86_space = false;
    memory_region_init_io(&s->iomem, obj, &sts_mem_ops, s, TYPE_PSP_STS, 4);

    sysbus_init_mmio(sbd, &s->iomem);
}

static void psp_sts_reset_hold(Object *obj, ResetType type)
{
    PSPStsState *s = PSP_STS(obj);

    s->psp_sts_val = 0;
}

static const VMStateDescription psp_sts_vmstate = {
    .name = TYPE_PSP_STS,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(psp_sts_val, PSPStsState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property properties[] = {
    DEFINE_PROP_BOOL("in_x86_space", PSPStsState, in_x86_space, false),
};

static void class_init(ObjectClass* oc, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    device_class_set_props(dc, properties);
    dc->vmsd = &psp_sts_vmstate;
    rc->phases.hold = psp_sts_reset_hold;
}

static const TypeInfo psp_sts_info = {
    .name = TYPE_PSP_STS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .class_init = class_init,
    .instance_init = psp_sts_init,
    .instance_size = sizeof(PSPStsState),
};

static void psp_sts_register_types(void)
{
    type_register_static(&psp_sts_info);
}
type_init(psp_sts_register_types);
