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
#include "hw/resettable.h"
#include "migration/vmstate.h"
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

static void psp_x86_set_mapping(PSPX86MapperState *s, uint32_t slot_id,
                                PSPX86Addr x86_addr)
{
    s->psp_x86_slots[slot_id].x86_addr = x86_addr;
    memory_region_set_alias_offset(&s->psp_x86_containers[slot_id], x86_addr);
}

static void psp_x86_map_slot(PSPX86MapperState* s, uint32_t slot_id)
{
    PSPX86Slot* slot = &s->psp_x86_slots[slot_id];

    PSPX86Addr x86_addr = (slot->ctrl_regs[X86BASE] & 0x3f) << 26 |
                          ((uint64_t)(slot->ctrl_regs[X86BASE] >> 6)) << 32;

    /*
     * The register can encode up to 2^58, the x86 space has 48 bits. An alias
     * past the end would not be rendered and the access would fall through to
     * the PSP-side devices below the slot, so drop the unmodelled high bits.
     */
    if (x86_addr >= X86_PHYS_ADDR_SPACE_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "psp-x86-mapper: slot %u X86BASE 0x%08x is above the "
                      "48-bit x86 space, masking\n",
                      slot_id, slot->ctrl_regs[X86BASE]);
        x86_addr &= X86_PHYS_ADDR_SPACE_SIZE - 1;
    }
    psp_x86_set_mapping(s, slot_id, x86_addr);
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

/*
 * Registers are 32 bits wide. A 4-byte access goes to the register as is.
 * A smaller access is placed in its byte lane, as the placeholder registers in
 * psp-misc.c do. An access that crosses the end of a register is not emulated.
 */
static uint64_t reg_read(PSPX86MapperState *s, hwaddr offset, unsigned int size,
                         uint32_t slot_id, PSPX86RegId reg_id)
{
    unsigned int lane = offset % sizeof(uint32_t);

    if (size == sizeof(uint32_t)) {
        return internal_state_read(s, slot_id, reg_id);
    }
    if (lane + size > sizeof(uint32_t)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PSP X86 Error: Read of size %u at 0x%" HWADDR_PRIx
                      " crosses a register\n",
                      size, offset);
        return 0;
    }
    return (internal_state_read(s, slot_id, reg_id) >> (lane * 8)) &
           MAKE_64BIT_MASK(0, size * 8);
}

static void reg_write(PSPX86MapperState *s, hwaddr offset, uint64_t value,
                      unsigned int size, uint32_t slot_id, PSPX86RegId reg_id)
{
    unsigned int lane = offset % sizeof(uint32_t);
    unsigned int shift = lane * 8;
    uint32_t mask;

    if (size == sizeof(uint32_t)) {
        internal_state_write(s, slot_id, reg_id, value);
        return;
    }
    if (lane + size > sizeof(uint32_t)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PSP X86 Error: Write of size %u at 0x%" HWADDR_PRIx
                      " crosses a register\n",
                      size, offset);
        return;
    }
    mask = (uint32_t)MAKE_64BIT_MASK(0, size * 8) << shift;
    internal_state_write(s, slot_id, reg_id,
                         (internal_state_read(s, slot_id, reg_id) & ~mask) |
                             (((uint32_t)value << shift) & mask));
}

static void ctrl_write(void* opaque, hwaddr offset, uint64_t value,
                       unsigned int size)
{
    PSPX86MapperState* s = PSP_X86_MAPPER(opaque);
    uint32_t slot_id = offset / (4 * sizeof(uint32_t));
    uint32_t reg_id = (offset % (4 * sizeof(uint32_t))) / 4;

    reg_write(s, offset, value, size, slot_id, reg_id);
}

static uint64_t ctrl_read(void* opaque, hwaddr offset, unsigned int size)
{
    PSPX86MapperState* s = PSP_X86_MAPPER(opaque);
    uint32_t slot_id = offset / (4 * sizeof(uint32_t));
    uint32_t reg_id = (offset % (4 * sizeof(uint32_t))) / 4;

    return reg_read(s, offset, size, slot_id, reg_id);
}

static void magic1_write(void* opaque, hwaddr offset, uint64_t value,
                         unsigned int size)
{
    PSPX86MapperState* s = PSP_X86_MAPPER(opaque);
    uint32_t slot_id = offset / sizeof(uint32_t);

    reg_write(s, offset, value, size, slot_id, X86_UNKNOWN4);
}

static uint64_t magic1_read(void* opaque, hwaddr offset, unsigned int size)
{
    PSPX86MapperState* s = PSP_X86_MAPPER(opaque);
    uint32_t slot_id = offset / sizeof(uint32_t);

    return reg_read(s, offset, size, slot_id, X86_UNKNOWN4);
}

static void magic2_write(void* opaque, hwaddr offset, uint64_t value,
                         unsigned int size)
{
    PSPX86MapperState* s = PSP_X86_MAPPER(opaque);
    uint32_t slot_id = offset / sizeof(uint32_t);

    reg_write(s, offset, value, size, slot_id, X86_UNKNOWN5);
}

static uint64_t magic2_read(void* opaque, hwaddr offset, unsigned int size)
{
    PSPX86MapperState* s = PSP_X86_MAPPER(opaque);
    uint32_t slot_id = offset / sizeof(uint32_t);

    return reg_read(s, offset, size, slot_id, X86_UNKNOWN5);
}

static const VMStateDescription psp_x86_slot_vmstate = {
    .name = "amd_psp.x86.slot",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(x86_addr, PSPX86Slot),
        VMSTATE_UINT64(psp_addr, PSPX86Slot),
        VMSTATE_UINT32_ARRAY(ctrl_regs, PSPX86Slot, PSP_X86_REG_COUNT),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * The x86 address of each slot is restored, and the container alias is
 * pointed at it again.
 */
static int mapper_post_load(void *opaque, int version_id)
{
    PSPX86MapperState *s = PSP_X86_MAPPER(opaque);

    for (unsigned int i = 0; i < PSP_X86_SLOT_COUNT; i++) {
        psp_x86_set_mapping(s, i, s->psp_x86_slots[i].x86_addr);
    }
    return 0;
}

static const VMStateDescription mapper_vmstate = {
    .name = TYPE_PSP_X86_MAPPER,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = mapper_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(psp_x86_slots, PSPX86MapperState,
                             PSP_X86_SLOT_COUNT, 1, psp_x86_slot_vmstate,
                             PSPX86Slot),
        VMSTATE_UINT32(magic3.value, PSPX86MapperState),
        VMSTATE_UINT32(magic3.counter, PSPX86MapperState),
        VMSTATE_END_OF_LIST()
    }
};

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
/*
 * Back to the power-on state: no control register set, each container at its
 * identity offset (see realize_mapping_slots()).
 */
static void mapper_reset_hold(Object *obj, ResetType type)
{
    PSPX86MapperState *s = PSP_X86_MAPPER(obj);

    for (unsigned int i = 0; i < PSP_X86_SLOT_COUNT; i++) {
        memset(s->psp_x86_slots[i].ctrl_regs, 0,
               sizeof(s->psp_x86_slots[i].ctrl_regs));
        psp_x86_set_mapping(s, i, i * PSP_X86_SLOT_SIZE);
    }
    s->magic3.value = 0;
    s->magic3.counter = 0;
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
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mapper_realize;
    dc->vmsd = &mapper_vmstate;
    rc->phases.hold = mapper_reset_hold;
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
