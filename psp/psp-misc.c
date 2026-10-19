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
 *
 * Special PSP device that covers various unknown MMIO regs.
 */
#include "qemu/osdep.h"

#include "psp/psp-misc.h"
#include "psp/psp-regs.h"

#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "trace.h"

/* TODO make values offset based only starting from mmio base */
/* TODO needs desperate rework. Rework to different classes or move into
 * its according file
 */

static void psp_misc_write(void* opaque, hwaddr offset, uint64_t value,
                           unsigned int size)
{
    /* TODO: Allow write to specific registers */

    trace_psp_misc_write_unimplemented(offset, value, size);
}

static uint64_t psp_misc_read(void* opaque, hwaddr offset, unsigned int size)
{
    PSPMiscState* misc = PSP_MISC(opaque);
    hwaddr phys_abs = misc->iomem.addr + offset;
    uint64_t value = 0;

    if (!psp_regs_lookup(misc->generation, PSP_REG_SPACE_PSP_MISC, phys_abs,
                         &value)) {
        trace_psp_misc_read_unimplemented(offset, size);
    } else {
        trace_psp_misc_read(offset, value, size);
    }

    return value;
}

static const MemoryRegionOps misc_mem_ops = {
    .read = psp_misc_read,
    .write = psp_misc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void psp_misc_init(Object* obj)
{
    PSPMiscState* s = PSP_MISC(obj);
    s->ident = NULL;
}

static void psp_misc_realize(DeviceState* dev, Error** errp)
{
    ERRP_GUARD();
    PSPMiscState* s = PSP_MISC(dev);

    if (s->mmio_size == 0) {
        error_setg(errp, "property 'mmio_size' not specified or zero");
        return;
    }

    if (s->ident == NULL) {
        error_setg(errp, "property 'ident' not specified");
        return;
    }
    if (s->generation >= PSP_GENERATION_MAX) {
        error_setg(errp, "property 'psp_misc_generation' not specified or invalid");
        return;
    }

    memory_region_init_io(&s->iomem, OBJECT(dev), &misc_mem_ops, s, s->ident,
                          0x44000000);

    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const Property properties[] = {
    DEFINE_PROP_UINT64("psp_misc_msize", PSPMiscState, mmio_size, 0),
    DEFINE_PROP_STRING("psp_misc_ident", PSPMiscState, ident),
    DEFINE_PROP_UINT32("psp_misc_generation", PSPMiscState, generation,
                       PSP_GENERATION_MAX),
};

static void psp_misc_class_init(ObjectClass* oc, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    dc->realize = psp_misc_realize;
    device_class_set_props(dc, properties);
}

static const TypeInfo psp_misc_info = {
    .name = TYPE_PSP_MISC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_init = psp_misc_init,
    .instance_size = sizeof(PSPMiscState),
    .class_init = psp_misc_class_init,

};

static void psp_misc_register_types(void)
{
    type_register_static(&psp_misc_info);
}

type_init(psp_misc_register_types);
