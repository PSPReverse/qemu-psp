/*
 * Copyright (C) 2024 Stefan Zabka <stefan@zabka.it>
 * SPDX-License-Identifier: GPL-2.0
 */

#include "psp/x86/wide-generic-port.h"

#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "trace.h"

static void psp_x86_misc_write(void* opaque, hwaddr addr, uint64_t value,
                               unsigned int size)
{
    LoggingInfo info = get_logging_info_for_addr(addr);
    trace_psp_x86_misc_write_unimplemented(addr, value, info.region_name,
                                           info.offset);
}

static const MemoryRegionOps misc_mem_ops = {
    .read = psp_x86_misc_read,
    .write = psp_x86_misc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static void psp_x86_misc_realize(DeviceState* dev, Error** errp)
{
    PSPX86MiscState* s = PSP_X86_MISC(dev);
    if (s->generation == PSP_GENERATION_MAX) {
        error_setg(errp,
                   "property 'generation' not specified for PSP X86 misc");
        return;
    }
    /* Init mmio */
    memory_region_init_io(&s->iomem, OBJECT(dev), &misc_mem_ops, s,
                          "psp-x86-misc", UINT64_MAX);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static Property psp_x86_misc_properties[] = {
    DEFINE_PROP_UINT32("psp_generation", PSPX86MiscState, generation,
                       PSP_GENERATION_MAX),
    DEFINE_PROP_LINK("owning-device", MMIODeviceState, owning_device,
                     TYPE_OWNING_DEVICE, OwningDeviceState*),
};

static void psp_x86_misc_class_init(ObjectClass* oc, void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    dc->realize = psp_x86_misc_realize;
    device_class_set_props(dc, psp_x86_misc_properties);
}

static const TypeInfo psp_x86_misc_info = {
    .name = TYPE_PSP_X86_MISC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PSPX86MiscState),
    .class_init = psp_x86_misc_class_init,
};

static void psp_misc_register_types(void)
{
    type_register_static(&psp_x86_misc_info);
}

type_init(psp_misc_register_types);
