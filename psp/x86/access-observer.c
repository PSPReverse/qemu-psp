/*
 * Copyright (C) 2024 Stefan Zabka <stefan@zabka.it>
 * SPDX-License-Identifier: GPL-2.0
 */
#include "qemu/osdep.h"

#include "psp/x86/access-observer.h"

#include "exec/cpu-defs.h" // Defined target_ulong for exit and cpu
#include "exec/cputlb.h"
#include "hw/core/cpu.h"
#include "hw/qdev-properties.h"
#include "libafl/cpu.h"
#include "libafl/exit.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "trace.h"

/** BEGIN ASPFuzz */
static AccessObserverState* global_access_observer = NULL;

/*
 * Overlay mode: shadow only the observed window of the x86 memory with an I/O
 * region instead of routing every access through the full-space region.
 */
static void overlay_remap(AccessObserverState* s)
{
    CPUState* cpu;

    memory_region_transaction_begin();
    if (s->overlay_mapped) {
        memory_region_del_subregion(&s->x86_memory->mr, &s->overlay_mr);
        s->overlay_mapped = false;
    }
    if (s->size) {
        memory_region_set_size(&s->overlay_mr, s->size);
        memory_region_add_subregion_overlap(&s->x86_memory->mr, s->base,
                                            &s->overlay_mr, 1000);
        s->overlay_mapped = true;
    }
    memory_region_transaction_commit();
    /* The CPUs cache the sections of the old layout in their TLBs */
    CPU_FOREACH(cpu) {
        tlb_flush(cpu);
    }
}

int32_t aspfuzz_access_observer_activate(hwaddr addr, hwaddr size);
int32_t aspfuzz_access_observer_activate(hwaddr addr, hwaddr size)
{
    AccessObserverState* s = global_access_observer;

    if (s == NULL) {
        return -1;
    }
    s->base = addr;
    s->size = size;
    if (s->overlay) {
        overlay_remap(s);
    }
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
    if (global_access_observer->overlay) {
        overlay_remap(global_access_observer);
    }
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
    if (addr >= s->base && addr - s->base < s->size) {
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
    if (addr >= s->base && addr - s->base < s->size) {
        s->observed_illegal_read = addr;
        s->pc_illegal_read = get_pc();
    }
    uint64_t value = 0;
    MemTxResult res = address_space_read(&s->x86_memory->as, addr,
                                         MEMTXATTRS_UNSPECIFIED, &value, size);
    if (res != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Access observer: read at 0x%" HWADDR_PRIx " failed\n",
                      addr);
        return 0;
    }
    return value;
}

/*
 * The overlay shadows the memory it observes, so it forwards to the RAM
 * through an unshadowed alias. Other devices in the window read as 0 and drop
 * writes.
 */
static void overlay_write(void* opaque, hwaddr offset, uint64_t value,
                          unsigned int size)
{
    AccessObserverState* s = PSP_X86_ACCESS_OBSERVER(opaque);
    /* Not s->base: a CPU can still be in an access that predates a reset */
    hwaddr addr = s->overlay_mr.addr + offset;

    s->observed_illegal_write = addr;
    s->pc_illegal_write = get_pc();
    address_space_write(&s->backing_as, addr, MEMTXATTRS_UNSPECIFIED, &value,
                        size);
}

static uint64_t overlay_read(void* opaque, hwaddr offset, unsigned int size)
{
    AccessObserverState* s = PSP_X86_ACCESS_OBSERVER(opaque);
    hwaddr addr = s->overlay_mr.addr + offset;
    uint64_t value = 0;

    s->observed_illegal_read = addr;
    s->pc_illegal_read = get_pc();
    address_space_read(&s->backing_as, addr, MEMTXATTRS_UNSPECIFIED, &value,
                       size);
    return value;
}

static const MemoryRegionOps overlay_ops = {
    .read = overlay_read,
    .write = overlay_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

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
    if (!s->x86_memory) {
        error_setg(errp,
                   "property 'x86_memory' not specified for PSP X86 Access "
                   "Observer");
        return;
    }
    if (s->overlay) {
        PSPX86State* x86 = s->x86_memory;

        memory_region_init_io(&s->overlay_mr, OBJECT(dev), &overlay_ops, s,
                              "access-observer-overlay", 1);
        memory_region_init(&s->backing_mr, OBJECT(dev),
                           "access-observer-backing",
                           X86_PHYS_ADDR_SPACE_SIZE);
        memory_region_init_alias(&s->secure_alias, OBJECT(dev),
                                 "access-observer-secure-dram",
                                 &x86->secure_dram, 0,
                                 memory_region_size(&x86->secure_dram));
        memory_region_add_subregion(&s->backing_mr, x86->secure_dram.addr,
                                    &s->secure_alias);
        memory_region_init_alias(&s->regular_alias, OBJECT(dev),
                                 "access-observer-regular-dram",
                                 &x86->regular_dram, 0,
                                 memory_region_size(&x86->regular_dram));
        memory_region_add_subregion(&s->backing_mr, x86->regular_dram.addr,
                                    &s->regular_alias);
        address_space_init(&s->backing_as, &s->backing_mr,
                           "access-observer backing");
    } else {
        /* Init x86 */
        memory_region_init_io(&s->mr, OBJECT(dev), &mem_ops, s,
                              "access-observer-mr", X86_PHYS_ADDR_SPACE_SIZE);
        sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mr);
    }
    global_access_observer = s;
}

static void instance_init(Object* obj)
{
    AccessObserverState* s = PSP_X86_ACCESS_OBSERVER(obj);
    s->base = 0;
    s->size = 0;
    object_property_add_uint64_ptr(obj, "observed_illegal_read",
                                   &s->observed_illegal_read,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "pc_illegal_read", &s->pc_illegal_read,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "observed_illegal_write",
                                   &s->observed_illegal_write,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "pc_illegal_write",
                                   &s->pc_illegal_write, OBJ_PROP_FLAG_READ);
}

/* "BASE,SIZE" in hex activates the window, "" resets it (for tests) */
static void set_window(Object* obj, const char* value, Error** errp)
{
    uint64_t base, size;
    const char* end;

    if (!*value) {
        aspfuzz_access_observer_reset();
    } else if (qemu_strtou64(value, &end, 16, &base) || *end != ',' ||
               qemu_strtou64(end + 1, NULL, 16, &size)) {
        error_setg(errp, "expected BASE,SIZE in hex");
    } else {
        aspfuzz_access_observer_activate(base, size);
    }
}

static const Property properties[] = {
    DEFINE_PROP_LINK("x86_memory", AccessObserverState, x86_memory,
                     TYPE_PSP_X86, PSPX86State*),
    DEFINE_PROP_BOOL("overlay", AccessObserverState, overlay, false),
};

static void class_init(ObjectClass* oc, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    dc->realize = realize;
    device_class_set_props(dc, properties);
    object_class_property_add_str(oc, "window", NULL, set_window);
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
