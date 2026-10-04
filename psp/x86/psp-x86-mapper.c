/*
 * Copyright (C) 2024 Stefan Zabka <stefan@zabka.it>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * @see PSPX86MapperState for a description of what this module does
 */

#include "qemu/osdep.h"

#include "psp/x86/psp-x86-mapper.h"

#include "system/address-spaces.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "trace.h"

#include "psp/psp.h"

static uint32_t internal_state_read(PSPX86MapperState* s, uint32_t slot_id,
                                    PSPX86RegId reg_id)
{
    if (slot_id >= PSP_X86_SLOT_COUNT || reg_id >= X86_REG_MAX) {
        trace_psp_x86_mapper_state_read_unimplemented(slot_id, reg_id, 0);
        return 0;
    }

    PSPX86Slot* slot = &s->psp_x86_slots[slot_id];
    uint32_t ret = slot->ctrl_regs[reg_id];
    trace_psp_x86_mapper_state_read(slot_id, reg_id, ret);

    return ret;
}

static void psp_x86_map_slot(PSPX86MapperState* s, uint32_t slot_id)
{
    PSPX86Slot* slot = &s->psp_x86_slots[slot_id];

    slot->x86_addr = (slot->ctrl_regs[X86BASE] & 0x3f) << 26 |
                     ((uint64_t)(slot->ctrl_regs[X86BASE] >> 6)) << 32;

    memory_region_set_alias_offset(&s->psp_x86_containers[slot_id],
                                   slot->x86_addr);
}

static void internal_state_write(PSPX86MapperState* s, uint32_t slot_id,
                                 PSPX86RegId reg_id, uint32_t val)
{
    if (slot_id >= PSP_X86_SLOT_COUNT || reg_id >= X86_REG_MAX) {
        trace_psp_x86_mapper_state_write_unimplemented(slot_id, reg_id, val);
        return;
    }

    PSPX86Slot* slot = &s->psp_x86_slots[slot_id];
    slot->ctrl_regs[reg_id] = val;

    if (reg_id == X86BASE) {
        psp_x86_map_slot(s, slot_id);
    }
    trace_psp_x86_mapper_state_write(slot_id, reg_id, val);
}

static void ctrl_write(void* opaque, hwaddr offset, uint64_t value,
                       unsigned int size)
{
    PSPX86MapperState* s = PSP_X86_MAPPER(opaque);
    uint32_t slot_id = offset / (4 * sizeof(uint32_t));
    uint32_t reg_id = (offset % (4 * sizeof(uint32_t))) / 4;

    if (size != sizeof(uint32_t)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PSP X86 Error: Unimplemented register access size %d\n",
                      size);
        return;
    }

    internal_state_write(s, slot_id, reg_id, value);
}

static uint64_t ctrl_read(void* opaque, hwaddr offset, unsigned int size)
{
    PSPX86MapperState* s = PSP_X86_MAPPER(opaque);
    uint32_t slot_id = offset / (4 * sizeof(uint32_t));
    uint32_t reg_id = (offset % (4 * sizeof(uint32_t))) / 4;

    if (size != sizeof(uint32_t)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PSP X86 Error: Unimplemented register access size %d\n",
                      size);
        return 0;
    }

    return internal_state_read(s, slot_id, reg_id);
}

static void magic1_write(void* opaque, hwaddr offset, uint64_t value,
                         unsigned int size)
{
    PSPX86MapperState* s = PSP_X86_MAPPER(opaque);
    uint32_t slot_id = offset / sizeof(uint32_t);
    uint32_t reg_id = X86_UNKNOWN4;

    if (size != sizeof(uint32_t)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PSP X86 Error: Unimplemented register"
                      " access size %d\n",
                      size);
        return;
    }

    internal_state_write(s, slot_id, reg_id, value);
}

static uint64_t magic1_read(void* opaque, hwaddr offset, unsigned int size)
{
    PSPX86MapperState* s = PSP_X86_MAPPER(opaque);
    uint32_t slot_id = offset / sizeof(uint32_t);
    uint32_t reg_id = X86_UNKNOWN4;

    if (size != sizeof(uint32_t)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PSP X86 Error: Unimplemented register"
                      " access size %d\n",
                      size);
        return 0;
    }

    return internal_state_read(s, slot_id, reg_id);
}

static void magic2_write(void* opaque, hwaddr offset, uint64_t value,
                         unsigned int size)
{
    PSPX86MapperState* s = PSP_X86_MAPPER(opaque);

    uint32_t slot_id = offset / sizeof(uint32_t);
    uint32_t reg_id = X86_UNKNOWN5;

    if (size != sizeof(uint32_t)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PSP X86 Error: Unimplemented register access size %d\n",
                      size);
        return;
    }

    internal_state_write(s, slot_id, reg_id, value);
}

static uint64_t magic2_read(void* opaque, hwaddr offset, unsigned int size)
{
    PSPX86MapperState* s = PSP_X86_MAPPER(opaque);

    uint32_t slot_id = offset / sizeof(uint32_t);
    uint32_t reg_id = X86_UNKNOWN5;

    if (size != sizeof(uint32_t)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PSP X86 Error: Unimplemented register access size %d\n",
                      size);
        return 0;
    }

    return internal_state_read(s, slot_id, reg_id);
}

static const MemoryRegionOps x86_ctlr1_ops = {
    .read = ctrl_read,
    .write = ctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static const MemoryRegionOps x86_magic1_ops = {
    .read = magic1_read,
    .write = magic1_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static const MemoryRegionOps x86_magic2_ops = {
    .read = magic2_read,
    .write = magic2_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static uint64_t magic3_read(void* opaque, hwaddr offset, unsigned int size)
{
    PSPX86MapperState* s = PSP_X86_MAPPER(opaque);
    if (offset != 0 || PSP_X86_MAGIC3_SIZE != size) {
        qemu_log_mask(
            LOG_UNIMP,
            "Misconfigured magic3 read at offset 0x%lx with size %d\n", offset,
            size);
        return 0;
    }
    trace_psp_x86_mapper_magic3_read(s->magic3.value);
    return s->magic3.value;
}

static void magic3_write(void* opaque, hwaddr offset, uint64_t value,
                         unsigned int size)
{
    PSPX86MapperState* s = PSP_X86_MAPPER(opaque);
    if (offset != 0 || PSP_X86_MAGIC3_SIZE != size) {
        qemu_log_mask(
            LOG_UNIMP,
            "Misconfigured magic3 write at offset 0x%lx with size %d\n", offset,
            size);
        return;
    }
    if (value == 0x3333) {
        s->magic3.counter++;
    }
    trace_psp_x86_mapper_magic3_write(value, s->magic3.counter);
    s->magic3.value = value;
}

static const MemoryRegionOps x86_magic3_ops = {
    .read = magic3_read,
    .write = magic3_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = PSP_X86_MAGIC3_SIZE,
    .impl.max_access_size = PSP_X86_MAGIC3_SIZE,
};

static void realize_mapping_slots(DeviceState* dev)
{
    PSPX86MapperState* s = PSP_X86_MAPPER(dev);
    MemoryRegion* target_mr = &s->x86_memory.mr;
    if (s->use_access_observer) {
        target_mr = &s->access_observer.mr;
    }

    for (unsigned int i = 0; i < PSP_X86_SLOT_COUNT; i++) {
        char name[PSP_X86_SLOT_NAME_LEN] = {0};
        /* The X86 MMIO region of the PSP */
        snprintf(name, PSP_X86_SLOT_NAME_LEN, "%s%d", PSP_X86_SLOT_NAME, i);
        /* Each container is an alias to the X86 addres space. Here we
         * initialize each container beginning with the address 0 in the x86
         * address space.
         */
        memory_region_init_alias(&s->psp_x86_containers[i], OBJECT(dev), name,
                                 target_mr, i * PSP_X86_SLOT_SIZE,
                                 PSP_X86_SLOT_SIZE);

        /* Map the containers to the PSP address space */
        hwaddr slot_offset = PSP_X86_MAPPING_BASE + i * PSP_X86_SLOT_SIZE;
        memory_region_add_subregion_overlap(get_system_memory(), slot_offset,
                                            &s->psp_x86_containers[i], 0);
    }
}
static void mapper_realize(DeviceState* dev, Error** errp)
{
    ERRP_GUARD();
    PSPX86MapperState* s = PSP_X86_MAPPER(dev);
    SysBusDevice* sbd = SYS_BUS_DEVICE(dev);

    if (s->generation == PSP_GENERATION_MAX) {
        error_setg(errp,
                   "property 'generation' not specified for PSP X86 Mapper");
        return;
    }
    // Initialize both
    qdev_prop_set_uint32(DEVICE(&s->x86_memory), "psp_generation",
                         s->generation);
    sysbus_realize(SYS_BUS_DEVICE(&s->x86_memory), errp);
    if (*errp) {
        return;
    }

    object_property_set_link(OBJECT(&s->access_observer), "x86_memory",
                             OBJECT(&s->x86_memory), errp);
    sysbus_realize(SYS_BUS_DEVICE(&s->access_observer), errp);
    if (*errp) {
        return;
    }
    /* The X86 control register regions */
    memory_region_init_io(&s->psp_x86_control, OBJECT(dev), &x86_ctlr1_ops, s,
                          "psp-x86-ctrl", PSP_X86_CTLR_SIZE);
    sysbus_init_mmio(sbd, &s->psp_x86_control);

    memory_region_init_io(&s->psp_x86_magic1, OBJECT(dev), &x86_magic1_ops, s,
                          "psp-x86-magic1", PSP_X86_MAGIC1_SIZE);
    sysbus_init_mmio(sbd, &s->psp_x86_magic1);

    memory_region_init_io(&s->psp_x86_magic2, OBJECT(dev), &x86_magic2_ops, s,
                          "psp-x86-magic2", PSP_X86_MAGIC2_SIZE);
    sysbus_init_mmio(sbd, &s->psp_x86_magic2);

    memory_region_init_io(&s->psp_x86_magic3, OBJECT(dev), &x86_magic3_ops, s,
                          "psp-x86-magic3", PSP_X86_MAGIC3_SIZE);
    sysbus_init_mmio(sbd, &s->psp_x86_magic3);

    realize_mapping_slots(dev);
}

static void mapper_init(Object* obj)
{
    PSPX86MapperState* s = PSP_X86_MAPPER(obj);
    object_initialize_child(obj, "access-observer", &s->access_observer,
                            TYPE_PSP_X86_ACCESS_OBSERVER);
    object_initialize_child(obj, "x86-full-memory", &s->x86_memory,
                            TYPE_PSP_X86);
}

static const Property properties[] = {
    DEFINE_PROP_UINT32("psp_generation", PSPX86MapperState, generation,
                       PSP_GENERATION_MAX),
    /* This only gets used when fuzzing with ASPFuzz, as it makes some of the
       emulation worse */
    DEFINE_PROP_BOOL("use_access_observer", PSPX86MapperState,
                     use_access_observer, false),
};

static void mapper_class_init(ObjectClass* oc, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    dc->realize = mapper_realize;
    device_class_set_props(dc, properties);
}

static const TypeInfo psp_x86_mapper_info = {
    .name = TYPE_PSP_X86_MAPPER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PSPX86MapperState),
    .class_init = mapper_class_init,
    .instance_init = mapper_init,
};

static void register_types(void) { type_register_static(&psp_x86_mapper_info); }
type_init(register_types);
