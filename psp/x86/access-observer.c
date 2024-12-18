/*
 * Copyright (C) 2024 Stefan Zabka <stefan@zabka.it>
 * SPDX-License-Identifier: GPL-2.0
 */

#include "psp/x86/access-observer.h"

#include "exec/cpu-defs.h" // Defined target_ulong for exit and cpu
#include "hw/qdev-properties.h"
#include "libafl/cpu.h"
#include "libafl/exit.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "trace.h"

/** BEGIN ASPFuzz */
static AccessObserverState* global_access_observer = NULL;

int32_t aspfuzz_access_observer_activate(hwaddr addr, hwaddr size);
int32_t aspfuzz_access_observer_activate(hwaddr addr, hwaddr size)
{
    if (global_access_observer == NULL) {
        return -1;
    }
    global_access_observer->base = addr;
    global_access_observer->size = size;
    return 0;
}

int32_t aspfuzz_access_observer_reset(void);
int32_t aspfuzz_access_observer_reset(void)
{
    if (global_access_observer == NULL) {
        return -1;
    }
    global_access_observer->base = 0;
    global_access_observer->size = 0;
    global_access_observer->observed_illegal_read = 0;
    global_access_observer->pc_illegal_read = 0;
    global_access_observer->observed_illegal_write = 0;
    global_access_observer->pc_illegal_write = 0;
    return 0;
}

int32_t aspfuzz_access_observer_status(bool get_read, hwaddr* addr, vaddr* pc);
int32_t aspfuzz_access_observer_status(bool get_read, hwaddr* addr, vaddr* pc)
{
    if (global_access_observer == NULL) {
        return -1;
    }
    if (get_read) {
        *addr = global_access_observer->observed_illegal_read;
        *pc = global_access_observer->pc_illegal_read;
    } else {
        *addr = global_access_observer->observed_illegal_write;
        *pc = global_access_observer->pc_illegal_write;
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
    AccessObserverState* s = PSP_X86_ACCESS_OBSERVER(opaque);
    if (addr >= s->base && addr < s->base + s->size) {
        s->observed_illegal_write = addr;
        s->pc_illegal_write = get_pc();
    }
    uint8_t buf[8]; // Maximum size for 64-bit data
    for (unsigned int i = 0; i < size; i++) {
        buf[i] = (value >> (8 * i)) & 0xFF;
    }
    address_space_write(&s->x86_memory->as, addr, MEMTXATTRS_UNSPECIFIED, &buf,
                        size);
}
static uint64_t cs_read(void* opaque, hwaddr addr, unsigned int size)
{
    AccessObserverState* s = PSP_X86_ACCESS_OBSERVER(opaque);
    if (addr >= s->base && addr < s->base + s->size) {
        s->observed_illegal_read = addr;
        s->pc_illegal_read = get_pc();
    }
    uint64_t value;
    address_space_read(&s->x86_memory->as, addr, MEMTXATTRS_UNSPECIFIED, &value,
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

    AccessObserverState* s = PSP_X86_ACCESS_OBSERVER(dev);
    /* Init x86 */
    memory_region_init_io(&s->mr, OBJECT(dev), &mem_ops, s,
                          "access-observer-mr", X86_PHYS_ADDR_SPACE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mr);
    global_access_observer = s;
}

static void instance_init(Object* obj)
{
    AccessObserverState* s = PSP_X86_ACCESS_OBSERVER(obj);
    s->base = 0;
    s->size = 0;
}

static Property properties[] = {
    DEFINE_PROP_LINK("x86_memory", AccessObserverState, x86_memory,
                     TYPE_PSP_X86, PSPX86State*),
};

static void class_init(ObjectClass* oc, void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    dc->realize = realize;
    device_class_set_props(dc, properties);
}

static const TypeInfo type_info = {
    .name = TYPE_PSP_X86_ACCESS_OBSERVER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AccessObserverState),
    .class_init = class_init,
    .instance_init = instance_init,
};

static void register_types(void) { type_register_static(&type_info); }

type_init(register_types);
