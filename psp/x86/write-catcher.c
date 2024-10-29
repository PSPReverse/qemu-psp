/*
 * Copyright (C) 2024 Stefan Zabka <stefan@zabka.it>
 * SPDX-License-Identifier: GPL-2.0
 */

#include "psp/x86/write-catcher.h"

#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "trace.h"

static void cs_write(void* opaque, hwaddr offset, uint64_t value,
                     unsigned int size)
{
    WriteCatcherState* s = PSP_X86_WRITE_CATCHER(opaque);
    s->observed_illegal_write = s->base + offset;
}
static uint64_t cs_read(void* opaque, hwaddr offset, unsigned int size)
{
    WriteCatcherState* s = PSP_X86_WRITE_CATCHER(opaque);
    uint64_t val = 0;
    s->observed_illegal_read = s->base + offset;
    return val;
}

static const MemoryRegionOps mem_ops = {
    .read = cs_read,
    .write = cs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static void realize(DeviceState* dev, Error** errp)
{
    WriteCatcherState* s = PSP_X86_WRITE_CATCHER(dev);
    /* Init mmio */
    memory_region_init_io(&s->iomem, OBJECT(dev), &mem_ops, s,
                          "psp-x86-write-catcher", 0);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void class_init(ObjectClass* oc, void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    dc->realize = realize;
}

static const TypeInfo type_info = {
    .name = TYPE_PSP_X86_WRITE_CATCHER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(WriteCatcherState),
    .class_init = class_init,
};

static void register_types(void) { type_register_static(&type_info); }

type_init(register_types);
