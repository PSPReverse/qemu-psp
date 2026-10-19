/*
 * AMD PSP emulation
 *
 * Copyright (C) 2020 Robert Buhren <robert@robertbuhren.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Special PSP device that covers various unknown MMIO regs.
 */
#include "qemu/osdep.h"

#include "psp/psp-misc.h"
#include "psp/psp-regs.h"
#include "psp/mmio/psp-timer.h"

#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "trace.h"

/* TODO make values offset based only starting from mmio base */
/* TODO needs desperate rework. Rework to different classes or move into
 * its according file
 */

/*
 * Registers that the boot path accesses but nothing models yet. They are
 * plain read/write-back storage, reset to 0 (what an unmodelled read returned
 * before). The stored values are placeholders, not hardware behaviour.
 */
static const hwaddr psp_misc_zen3_placeholder_addr[] = {
    0x3010004, 0x3010058, 0x3260028, 0x32000dc, 0x3200054,
    0x320021c, 0x320022c, 0x30109a4, 0x301070c, 0x3010a44,
};

_Static_assert(ARRAY_SIZE(psp_misc_zen3_placeholder_addr) ==
                   PSP_MISC_ZEN3_PLACEHOLDER_COUNT,
               "zen3 placeholder table and state size must match");

/*
 * Placeholders on every generation, by design: the fuzzer needs them on Zen2
 * too. A read after a write returns the written value on all generations.
 * No value in psp_regs_lookup() changes, because that lookup takes
 * precedence. 0x3210044 is not in this table: it holds a pointer, and the
 * firmware's 0x70..0x73 interrupt handler stores 1 << (id - 0x70) at the
 * address it points to. That target is not modelled.
 */
static const hwaddr psp_misc_placeholder_addr[] = {
    0x3200a00, 0x3200a04, 0x3200a08,
    0x30109f0, 0x30109f4, 0x30109f8,
};

_Static_assert(ARRAY_SIZE(psp_misc_placeholder_addr) ==
                   PSP_MISC_PLACEHOLDER_COUNT,
               "placeholder table and state size must match");

/* @return storage for the placeholder word at addr (4-byte aligned), or NULL */
static uint32_t *psp_misc_placeholder_slot(PSPMiscState *misc, hwaddr addr)
{
    if (misc->generation == ZEN3) {
        for (size_t i = 0; i < ARRAY_SIZE(psp_misc_zen3_placeholder_addr);
             i++) {
            if (psp_misc_zen3_placeholder_addr[i] == addr) {
                return &misc->zen3_placeholder[i];
            }
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(psp_misc_placeholder_addr); i++) {
        if (psp_misc_placeholder_addr[i] == addr) {
            return &misc->placeholder[i];
        }
    }
    return NULL;
}

static void psp_misc_write(void* opaque, hwaddr offset, uint64_t value,
                           unsigned int size)
{
    PSPMiscState* misc = PSP_MISC(opaque);
    hwaddr phys_abs = misc->iomem.addr + offset;
    uint32_t *slot = psp_misc_placeholder_slot(misc, phys_abs & ~(hwaddr)3);
    unsigned shift = (phys_abs & 3) * 8;
    uint32_t mask = (uint32_t)MAKE_64BIT_MASK(0, size * 8) << shift;

    if (slot) {
        *slot = (*slot & ~mask) | (((uint32_t)value << shift) & mask);
        trace_psp_misc_write(offset, value, size);
    } else {
        trace_psp_misc_write_unimplemented(offset, value, size);
    }
}

/*
 * Low and high word of the 64-bit system tick counter, see
 * psp_timer_system_ticks(). Each read takes its own snapshot, so the two words
 * can disagree if the counter carries between them. Zero unless counter_hz is
 * set on timer1, which keeps the default reads unmodelled.
 */
static bool psp_misc_system_ticks_word(hwaddr addr, unsigned int size,
                                       uint64_t *val)
{
    uint64_t ticks;

    if (addr != 0x032001f0 && addr != 0x032001f4) {
        return false;
    }
    ticks = psp_timer_system_ticks();
    if (ticks == 0) {
        return false;
    }
    ticks = addr == 0x032001f0 ? (uint32_t)ticks : (uint32_t)(ticks >> 32);
    *val = ticks & MAKE_64BIT_MASK(0, size * 8);
    return true;
}

static uint64_t psp_misc_read(void* opaque, hwaddr offset, unsigned int size)
{
    PSPMiscState* misc = PSP_MISC(opaque);
    hwaddr phys_abs;
    hwaddr phys_base;
    uint64_t value;
    uint32_t *slot;

    phys_base = misc->iomem.addr;
    phys_abs = phys_base + offset;
    value = 0;

    slot = psp_misc_placeholder_slot(misc, phys_abs & ~(hwaddr)3);
    if (psp_misc_system_ticks_word(phys_abs, size, &value) ||
        psp_regs_lookup(misc->generation, PSP_REG_SPACE_PSP_MISC, phys_abs,
                        &value)) {
        trace_psp_misc_read(offset, value, size);
    } else if (slot) {
        value = (*slot >> ((phys_abs & 3) * 8)) & MAKE_64BIT_MASK(0, size * 8);
        trace_psp_misc_read(offset, value, size);
    } else {
        trace_psp_misc_read_unimplemented(offset, size);
    }

    return value;
}

static const MemoryRegionOps misc_mem_ops = {
    .read = psp_misc_read,
    .write = psp_misc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void psp_misc_init(Object* obj)
{
    PSPMiscState* s = PSP_MISC(obj);
    s->ident = NULL;
}

static void psp_misc_realize(DeviceState* dev, Error** errp)
{
    ERRP_GUARD();
    PSPMiscState* s = PSP_MISC(dev);

    if (s->mmio_size == 0) {
        error_setg(errp, "property 'mmio_size' not specified or zero");
        return;
    }

    if (s->ident == NULL) {
        error_setg(errp, "property 'ident' not specified");
        return;
    }
    if (s->generation >= PSP_GENERATION_MAX) {
        error_setg(errp,
                   "property 'psp_misc_generation' not specified or invalid");
        return;
    }

    memory_region_init_io(&s->iomem, OBJECT(dev), &misc_mem_ops, s, s->ident,
                          0x44000000);

    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static void psp_misc_reset_hold(Object *obj, ResetType type)
{
    PSPMiscState *s = PSP_MISC(obj);

    memset(s->zen3_placeholder, 0, sizeof(s->zen3_placeholder));
    memset(s->placeholder, 0, sizeof(s->placeholder));
}

static const VMStateDescription psp_misc_vmstate = {
    .name = "psp-misc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(zen3_placeholder, PSPMiscState,
                             PSP_MISC_ZEN3_PLACEHOLDER_COUNT),
        VMSTATE_UINT32_ARRAY(placeholder, PSPMiscState,
                             PSP_MISC_PLACEHOLDER_COUNT),
        VMSTATE_END_OF_LIST()
    }
};

static const Property properties[] = {
    DEFINE_PROP_UINT64("psp_misc_msize", PSPMiscState, mmio_size, 0),
    DEFINE_PROP_STRING("psp_misc_ident", PSPMiscState, ident),
    DEFINE_PROP_UINT32("psp_misc_generation", PSPMiscState, generation,
                       PSP_GENERATION_MAX),
};

static void psp_misc_class_init(ObjectClass* oc, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    dc->realize = psp_misc_realize;
    dc->vmsd = &psp_misc_vmstate;
    rc->phases.hold = psp_misc_reset_hold;
    device_class_set_props(dc, properties);
}

static const TypeInfo psp_misc_info = {
    .name = TYPE_PSP_MISC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_init = psp_misc_init,
    .instance_size = sizeof(PSPMiscState),
    .class_init = psp_misc_class_init,

};

static void psp_misc_register_types(void)
{
    type_register_static(&psp_misc_info);
}

type_init(psp_misc_register_types);
