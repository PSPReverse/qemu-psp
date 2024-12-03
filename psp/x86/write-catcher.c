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

/** BEGIN ASPFuzz */
static WriteCatcherState* global_write_catcher = NULL;

int32_t aspfuzz_write_catcher_activate(hwaddr addr, hwaddr size);
int32_t aspfuzz_write_catcher_activate(hwaddr addr, hwaddr size)
{
    if (global_write_catcher == NULL) {
        return -1;
    }
    global_write_catcher->base = addr;
    global_write_catcher->size = size;
    return 0;
}

int32_t aspfuzz_write_catcher_reset(void);
int32_t aspfuzz_write_catcher_reset(void)
{
    if (global_write_catcher == NULL) {
        return -1;
    }
    global_write_catcher->base = 0;
    global_write_catcher->size = 0;
    global_write_catcher->observed_illegal_read = 0;
    global_write_catcher->pc_illegal_read = 0;
    global_write_catcher->observed_illegal_write = 0;
    global_write_catcher->pc_illegal_write = 0;
    return 0;
}

int32_t aspfuzz_write_catcher_status(bool get_read, hwaddr* addr, vaddr* pc);
int32_t aspfuzz_write_catcher_status(bool get_read, hwaddr* addr, vaddr* pc)
{
    if (global_write_catcher == NULL) {
        return -1;
    }
    if (get_read) {
        *addr = global_write_catcher->observed_illegal_read;
        *pc = global_write_catcher->pc_illegal_read;
    } else {
        *addr = global_write_catcher->observed_illegal_write;
        *pc = global_write_catcher->pc_illegal_write;
    }
    return 0;
}
/** END ASPFuzz */

static vaddr get_pc(void)
{
    CPUState* state = libafl_qemu_current_cpu();

    if (!state) {
        qemu_log_mask(LOG_UNIMP, "Couldn't get CPU state in get_pc\n");
        return 0;
    }
    CPUClass* cc = CPU_GET_CLASS(state);
    if (!cc) {
        qemu_log_mask(LOG_UNIMP, "Couldn't get CPUClass in get_pc\n");
        return 0;
    }
    return cc->get_pc(state);
}
static void cs_write(void* opaque, hwaddr addr, uint64_t value,
                     unsigned int size)
{
    WriteCatcherState* s = PSP_X86_WRITE_CATCHER(opaque);
    if (addr >= s->base && addr < s->base + s->size) {
        s->observed_illegal_write = addr;
        s->pc_illegal_write = get_pc();
    }
    uint8_t buf[8]; // Maximum size for 64-bit data
    for (unsigned int i = 0; i < size; i++) {
        buf[i] = (value >> (8 * i)) & 0xFF;
    }
    address_space_write(&s->x86_memory.as, addr, MEMTXATTRS_UNSPECIFIED, &buf,
                        size);
}
static uint64_t cs_read(void* opaque, hwaddr addr, unsigned int size)
{
    WriteCatcherState* s = PSP_X86_WRITE_CATCHER(opaque);
    if (addr >= s->base && addr < s->base + s->size) {
        s->observed_illegal_read = s->base + addr;
        s->pc_illegal_read = get_pc();
    }
    uint64_t value;
    address_space_read(&s->x86_memory.as, addr, MEMTXATTRS_UNSPECIFIED, &value,
                       size);
    return value;
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
    ERRP_GUARD();

    WriteCatcherState* s = PSP_X86_WRITE_CATCHER(dev);
    /* Init x86 */
    memory_region_init_io(&s->mr, OBJECT(dev), &mem_ops, s,
                          "psp-x86-write-catcher", X86_PHYS_ADDR_SPACE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mr);

    qdev_prop_set_uint32(DEVICE(&s->x86_memory), "psp_generation",
                         s->generation);
    sysbus_realize(SYS_BUS_DEVICE(&s->x86_memory), errp);
    if (*errp) {
        return;
    }
    global_write_catcher = s;
}

static void instance_init(Object* obj)
{
    WriteCatcherState* s = PSP_X86_WRITE_CATCHER(obj);
    object_initialize_child(obj, "x86-full-memory", &s->x86_memory,
                            TYPE_PSP_X86);
    s->base = 0;
    s->size = 0;
}

static Property properties[] = {
    DEFINE_PROP_UINT32("psp_generation", WriteCatcherState, generation,
                       PSP_GENERATION_MAX),
};

static void class_init(ObjectClass* oc, void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    dc->realize = realize;
    device_class_set_props(dc, properties);
}

static const TypeInfo type_info = {
    .name = TYPE_PSP_X86_WRITE_CATCHER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(WriteCatcherState),
    .class_init = class_init,
    .instance_init = instance_init,
};

static void register_types(void) { type_register_static(&type_info); }

type_init(register_types);
