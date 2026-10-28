/*
 * Copyright (C) 2024 Stefan Zabka <stefan@zabka.it>
 * SPDX-License-Identifier: GPL-2.0
 */
#include "qemu/osdep.h"

#include "psp/x86/access-observer.h"

#include "exec/cpu-defs.h" // Defined target_ulong for exit and cpu
#include "hw/qdev-properties.h"
#include "libafl/cpu.h"
#include "libafl/exit.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/cutils.h"
#include "trace.h"

/** BEGIN ASPFuzz */
static AccessObserverState* global_access_observer = NULL;

static void clear_excludes(AccessObserverState* s)
{
    s->exclude_count = 0;
    memset(s->exclude, 0, sizeof(s->exclude));
}

static int observer_exclude(AccessObserverState* s, hwaddr addr, hwaddr size)
{
    if (s->exclude_count >= ACCESS_OBSERVER_MAX_EXCLUDES) {
        return -1;
    }
    s->exclude[s->exclude_count].base = addr;
    s->exclude[s->exclude_count].size = size;
    s->exclude_count++;
    return 0;
}

/* Illegal: inside the activated window and in none of the exclude ranges */
static bool is_illegal(const AccessObserverState* s, hwaddr addr)
{
    if (addr < s->base || addr - s->base >= s->size) {
        return false;
    }
    for (unsigned int i = 0; i < s->exclude_count; i++) {
        if (addr >= s->exclude[i].base &&
            addr - s->exclude[i].base < s->exclude[i].size) {
            return false;
        }
    }
    return true;
}

static void clear_touched(AccessObserverState* s)
{
    memset(s->touched, 0, sizeof(s->touched));
    memset(s->touched_index, 0, sizeof(s->touched_index));
    s->touched_count = 0;
    s->touched_overflow = 0;
    s->last_touched = -1;
}

static vaddr get_pc(void);

/*
 * Accesses are on the hot path, so lookup is O(1): consecutive accesses mostly
 * hit the same page (last_touched), otherwise probe a hash kept at most half
 * full (2 slots per entry) instead of scanning up to 256 entries.
 */
static void record_touch(AccessObserverState* s, hwaddr addr, uint8_t flag)
{
    uint64_t page = addr & ~(hwaddr)0xfff;
    uint32_t slot;
    uint32_t idx;

    if (s->last_touched >= 0 && s->touched[s->last_touched].page == page) {
        s->touched[s->last_touched].flags |= flag;
        return;
    }
    slot = (uint32_t)((page >> 12) * 0x9E3779B1u) %
           ACCESS_OBSERVER_TOUCHED_SLOTS;
    while ((idx = s->touched_index[slot]) != 0) {
        if (s->touched[idx - 1].page == page) {
            s->touched[idx - 1].flags |= flag;
            s->last_touched = idx - 1;
            return;
        }
        slot = (slot + 1) % ACCESS_OBSERVER_TOUCHED_SLOTS;
    }
    if (s->touched_count >= ACCESS_OBSERVER_MAX_TOUCHED) {
        s->touched_overflow++;
        return;
    }
    idx = s->touched_count++;
    s->touched[idx].page = page;
    s->touched[idx].pc = get_pc();
    s->touched[idx].flags = flag;
    s->touched_index[slot] = idx + 1;
    s->last_touched = idx;
}

static void observer_activate(AccessObserverState* s, hwaddr addr, hwaddr size)
{
    s->base = addr;
    s->size = size;
    clear_excludes(s);
    clear_touched(s);
}

static void observer_reset(AccessObserverState* s)
{
    s->base = 0;
    s->size = 0;
    clear_excludes(s);
    s->observed_illegal_read = 0;
    s->pc_illegal_read = 0;
    s->illegal_read_hits = 0;
    s->observed_illegal_write = 0;
    s->pc_illegal_write = 0;
    s->illegal_write_hits = 0;
    clear_touched(s);
}

int32_t aspfuzz_access_observer_activate(hwaddr addr, hwaddr size);
int32_t aspfuzz_access_observer_activate(hwaddr addr, hwaddr size)
{
    if (global_access_observer == NULL) {
        return -1;
    }
    observer_activate(global_access_observer, addr, size);
    return 0;
}

int32_t aspfuzz_access_observer_exclude(hwaddr addr, hwaddr size);
int32_t aspfuzz_access_observer_exclude(hwaddr addr, hwaddr size)
{
    if (global_access_observer == NULL) {
        return -1;
    }
    return observer_exclude(global_access_observer, addr, size);
}

int32_t aspfuzz_access_observer_reset(void);
int32_t aspfuzz_access_observer_reset(void)
{
    if (global_access_observer == NULL) {
        return -1;
    }
    observer_reset(global_access_observer);
    return 0;
}

/* First illegal access of the kind since the last reset */
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

int32_t aspfuzz_access_observer_hits(bool get_read, uint64_t* count);
int32_t aspfuzz_access_observer_hits(bool get_read, uint64_t* count)
{
    if (global_access_observer == NULL) {
        return -1;
    }
    *count = get_read ? global_access_observer->illegal_read_hits
                      : global_access_observer->illegal_write_hits;
    return 0;
}

/* Distinct pages touched since the last activate/reset, first-touch order */
int32_t aspfuzz_access_observer_touched(AspfuzzTouchedPage* buf, uint32_t max,
                                        uint32_t* total);
int32_t aspfuzz_access_observer_touched(AspfuzzTouchedPage* buf, uint32_t max,
                                        uint32_t* total)
{
    if (global_access_observer == NULL) {
        return -1;
    }
    uint32_t n = MIN(max, global_access_observer->touched_count);
    memcpy(buf, global_access_observer->touched, n * sizeof(*buf));
    *total = global_access_observer->touched_count;
    return n;
}

/* Pages dropped because the table of 256 was full */
int32_t aspfuzz_access_observer_touched_overflow(uint64_t* count);
int32_t aspfuzz_access_observer_touched_overflow(uint64_t* count)
{
    if (global_access_observer == NULL) {
        return -1;
    }
    *count = global_access_observer->touched_overflow;
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
    if (is_illegal(s, addr)) {
        record_touch(s, addr, ACCESS_OBSERVER_TOUCH_WRITE);
        /* The counter, not the address, tells whether this is the first */
        if (s->illegal_write_hits++ == 0) {
            s->observed_illegal_write = addr;
            s->pc_illegal_write = get_pc();
        }
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
    if (is_illegal(s, addr)) {
        record_touch(s, addr, ACCESS_OBSERVER_TOUCH_READ);
        if (s->illegal_read_hits++ == 0) {
            s->observed_illegal_read = addr;
            s->pc_illegal_read = get_pc();
        }
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
    /* Init x86 */
    memory_region_init_io(&s->mr, OBJECT(dev), &mem_ops, s,
                          "access-observer-mr", X86_PHYS_ADDR_SPACE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mr);
    global_access_observer = s;
}

static bool parse_range(const char* value, uint64_t* base, uint64_t* size,
                        Error** errp)
{
    const char* end;

    if (qemu_strtou64(value, &end, 16, base) < 0 || *end != ',' ||
        qemu_strtou64(end + 1, NULL, 16, size) < 0) {
        error_setg(errp, "expected \"BASE,SIZE\" in hex, got '%s'", value);
        return false;
    }
    return true;
}

/*
 * Write-only properties for qtest, mirroring the aspfuzz_access_observer_*
 * C ABI which a test cannot call: "window" is activate, "exclude" is exclude
 * (both "BASE,SIZE" in hex) and "reset" (true) is reset.
 */
static void set_window(Object* obj, const char* value, Error** errp)
{
    uint64_t base, size;

    if (parse_range(value, &base, &size, errp)) {
        observer_activate(PSP_X86_ACCESS_OBSERVER(obj), base, size);
    }
}

static void set_reset(Object* obj, bool value, Error** errp)
{
    if (value) {
        observer_reset(PSP_X86_ACCESS_OBSERVER(obj));
    }
}

static void set_exclude(Object* obj, const char* value, Error** errp)
{
    AccessObserverState* s = PSP_X86_ACCESS_OBSERVER(obj);
    uint64_t base, size;

    if (!parse_range(value, &base, &size, errp)) {
        return;
    }
    if (observer_exclude(s, base, size) < 0) {
        error_setg(errp, "all %d exclude ranges are in use",
                   ACCESS_OBSERVER_MAX_EXCLUDES);
    }
}

/* "page:flags:pc,..." all hex, first-touch order */
static char* get_touched(Object* obj, Error** errp)
{
    AccessObserverState* s = PSP_X86_ACCESS_OBSERVER(obj);
    GString* str = g_string_new(NULL);

    for (uint32_t i = 0; i < s->touched_count; i++) {
        g_string_append_printf(str, "%s%" PRIx64 ":%x:%" PRIx64, i ? "," : "",
                               s->touched[i].page, s->touched[i].flags,
                               s->touched[i].pc);
    }
    return g_string_free(str, false);
}

static void instance_init(Object* obj)
{
    AccessObserverState* s = PSP_X86_ACCESS_OBSERVER(obj);
    s->base = 0;
    s->size = 0;
    s->last_touched = -1;
    object_property_add_uint64_ptr(obj, "illegal_read_hits",
                                   &s->illegal_read_hits,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "illegal_write_hits",
                                   &s->illegal_write_hits,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "illegal_read_addr",
                                   &s->observed_illegal_read,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "illegal_write_addr",
                                   &s->observed_illegal_write,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "touched_pages", &s->touched_count,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "touched_overflow",
                                   &s->touched_overflow, OBJ_PROP_FLAG_READ);
    object_property_add_str(obj, "touched", get_touched, NULL);
    object_property_add_str(obj, "window", NULL, set_window);
    object_property_add_str(obj, "exclude", NULL, set_exclude);
    object_property_add_bool(obj, "reset", NULL, set_reset);
}

static const Property properties[] = {
    DEFINE_PROP_LINK("x86_memory", AccessObserverState, x86_memory,
                     TYPE_PSP_X86, PSPX86State*),
};

static void class_init(ObjectClass* oc, const void* data)
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
