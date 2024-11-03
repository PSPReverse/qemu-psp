/*
 * Copyright (C) 2024 Stefan Zabka <stefan@zabka.it>
 * SPDX-License-Identifier: GPL-2.0
 */

#include "psp/x86/write-catcher.h"

#include "exec/cpu-defs.h" // Defined target_ulong for exit and cpu
#include "hw/qdev-properties.h"
#include "libafl/cpu.h"
#include "libafl/exit.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "trace.h"

static void access_catcher_trigger_breakpoint(void)
{
    CPUState* state = libafl_qemu_current_cpu();
    if (!state) {
        qemu_log_mask(LOG_UNIMP, "Couldn't get CPU state in WriteCatcher\n");
        return;
    }
    libafl_qemu_trigger_breakpoint(state);
}
static void cs_write(void* opaque, hwaddr offset, uint64_t value,
                     unsigned int size)
{
    WriteCatcherState* s = PSP_X86_WRITE_CATCHER(opaque);
    s->observed_illegal_write = s->base + offset;
    access_catcher_trigger_breakpoint();
}
static uint64_t cs_read(void* opaque, hwaddr offset, unsigned int size)
{
    WriteCatcherState* s = PSP_X86_WRITE_CATCHER(opaque);
    uint64_t val = 0;
    s->observed_illegal_read = s->base + offset;
    access_catcher_trigger_breakpoint();

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
