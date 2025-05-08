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

#include "psp/psp-irq.h"

#include "exec/address-spaces.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "trace.h"

static void psp_irq_write(void* opaque, hwaddr offset, uint64_t value,
                          unsigned int size)
{
    PSPIrqState* irq = PSP_IRQ(opaque);
    unsigned int old_val = irq->bitfield[offset >> 2];

    if (offset >= 0x10) {
        trace_psp_irq_write_unimplemented(offset, value, size);

        return;
    }
    if (old_val == value)
        return;

    // we are inside the interrupt_bitfield
    unsigned int diff = old_val ^ value;
    unsigned int idx = __builtin_ctz(diff);
    if ((old_val & diff) != 0) {
        // bit was cleared
        trace_psp_irq_clear_bit(offset * 8 + idx);
    } else {
        // bit was set
        trace_psp_irq_set_bit(offset * 8 + idx);
    }
    // trace_psp_irq_write_bitfield(offset, value, size);
    irq->bitfield[offset >> 2] = value;
}

static uint64_t psp_irq_read(void* opaque, hwaddr offset, unsigned int size)
{
    PSPIrqState* irq = PSP_IRQ(opaque);
    uint64_t value;

    value = 0;

    if (offset < 0x10) {
        // we are inside the interrupt_bitfield
        value = irq->bitfield[offset >> 2];
        trace_psp_irq_read_bitfield(offset, value, size);
    } else {
        trace_psp_irq_read_unimplemented(offset, size);
    }

    return value;
}

static const MemoryRegionOps irq_mem_ops = {
    .read = psp_irq_read,
    .write = psp_irq_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void psp_irq_init(Object* obj)
{
    PSPIrqState* s = PSP_IRQ(obj);
    s->ident = NULL;
}

static void psp_irq_realize(DeviceState* dev, Error** errp)
{
    ERRP_GUARD();
    PSPIrqState* s = PSP_IRQ(dev);

    // clear the bitfield
    memset(&s->bitfield, 0, sizeof(s->bitfield));

    if (s->ident == NULL) {
        error_setg(errp, "property 'ident' not specified");
        return;
    }
    if (s->generation == PSP_GENERATION_MAX) {
        error_setg(errp, "property 'psp_irq_generation' not specified");
        return;
    }

    memory_region_init_io(&s->iomem, OBJECT(dev), &irq_mem_ops, s, s->ident,
                          0x100);

    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static Property properties[] = {
    DEFINE_PROP_STRING("psp_irq_ident", PSPIrqState, ident),
    DEFINE_PROP_UINT32("psp_irq_generation", PSPIrqState, generation,
                       PSP_GENERATION_MAX),
};

static void psp_irq_class_init(ObjectClass* oc, void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    dc->realize = psp_irq_realize;
    device_class_set_props(dc, properties);
}

static const TypeInfo psp_irq_info = {
    .name = TYPE_PSP_IRQ,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_init = psp_irq_init,
    .instance_size = sizeof(PSPIrqState),
    .class_init = psp_irq_class_init,

};

static void psp_irq_register_types(void)
{
    type_register_static(&psp_irq_info);
}

type_init(psp_irq_register_types);
