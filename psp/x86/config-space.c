/*
 * Copyright (C) 2024 Stefan Zabka <stefan@zabka.it>
 * SPDX-License-Identifier: GPL-2.0
 */
#include "qemu/osdep.h"

#include "psp/x86/config-space.h"

#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "trace.h"

/**
 * A guessed count of available devices
 * The correct value would have to be provided by MSRC001_0058
 * (Core::X86::Msr::MmioCfgBaseAddr) in bits 5:2 Each Bus has 1MB of size in
 * config space
 */
#define BUS_COUNT 8

typedef struct PciIdentifier {
    uint32_t offset : 12;
    uint32_t function : 3;
    uint32_t device : 5;
    uint32_t bus : 8;
    uint32_t filler : 4;
    uint32_t filler2;
} PciIdentifier;

static void cs_write(void* opaque, hwaddr offset, uint64_t value,
                     unsigned int size)
{
    PciIdentifier* id = (PciIdentifier*)&offset;
    trace_psp_x86_config_space_write_unimplemented(
        id->bus, id->device, id->function, id->offset, value);
}
static uint64_t cs_read(void* opaque, hwaddr offset, unsigned int size)
{
    PciIdentifier* id = (PciIdentifier*)&offset;
    uint64_t val = 0;
    trace_psp_x86_config_space_read_unimplemented(
        id->bus, id->device, id->function, id->offset, val);
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
    PSPX86ConfigSpaceState* s = PSP_X86_CONFIG_SPACE(dev);
    if (s->generation == PSP_GENERATION_MAX) {
        error_setg(errp,
                   "property 'generation' not specified for PSP X86 misc");
        return;
    }
    /* Init mmio */
    memory_region_init_io(&s->iomem, OBJECT(dev), &mem_ops, s,
                          "psp-x86-config-space",
                          BUS_COUNT * 1024 * 1024); // 1MB per bus
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const Property properties[] = {
    DEFINE_PROP_UINT32("psp_generation", PSPX86ConfigSpaceState, generation,
                       PSP_GENERATION_MAX),
};

static void class_init(ObjectClass* oc, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    dc->realize = realize;
    device_class_set_props(dc, properties);
}

static const TypeInfo type_info = {
    .name = TYPE_PSP_X86_CONFIG_SPACE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PSPX86ConfigSpaceState),
    .class_init = class_init,
};

static void register_types(void) { type_register_static(&type_info); }

type_init(register_types);
