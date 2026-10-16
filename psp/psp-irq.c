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

#include "system/address-spaces.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"
#include "hw/resettable.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "trace.h"

/*
 * Assert the core line whenever any interrupt source is pending, deassert it
 * once the firmware has ACKed them all. This is the delivery half the device
 * historically lacked (it only tracked the pending bitfield).
 */
static void psp_irq_update(PSPIrqState* irq)
{
    bool pending = irq->irq_buffer[0] | irq->irq_buffer[1] |
                   irq->irq_buffer[2] | irq->irq_buffer[3];
    qemu_set_irq(irq->parent_irq, pending);
}

/*
 * GPIO input: devices (mailbox doorbell, timers, ...) connect an output line
 * here, one per interrupt source (group * 32 + bit). Raising a line marks the
 * source pending and asserts the core line; the firmware clears it via the
 * FIQ_HANDLED ACK register.
 */
static void psp_irq_set_irq(void* opaque, int n, int level)
{
    PSPIrqState* irq = PSP_IRQ(opaque);
    uint32_t mask = 1u << (n & 31);

    if (level) {
        irq->irq_buffer[n >> 5] |= mask;
        trace_psp_irq_set_bit(n);
    } else {
        irq->irq_buffer[n >> 5] &= ~mask;
        trace_psp_irq_clear_bit(n);
    }
    psp_irq_update(irq);
}


static void psp_irq_write_ccp_queue(PSPIrqState* irq, hwaddr offset, uint64_t value) {
    unsigned int old_val = irq->ccp_queue_bitfield[offset >> 2];
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
    irq->ccp_queue_bitfield[offset >> 2] = value;
    return;
}

static void psp_irq_write_fiq(PSPIrqState* irq, hwaddr offset, uint64_t value, unsigned int size) {
    switch (offset) {
        case FIQ_HANDLED_BITFIELD_OFFSET:
            irq->irq_buffer[0] &= ~value;
            trace_psp_irq_clear_bit(__builtin_ctz(value));
            break;
        case FIQ_HANDLED_BITFIELD_OFFSET+4:
            irq->irq_buffer[1] &= ~value;
            trace_psp_irq_clear_bit(32+__builtin_ctz(value));
            break;
        case FIQ_HANDLED_BITFIELD_OFFSET+8:
            irq->irq_buffer[2] &= ~value;
            trace_psp_irq_clear_bit(64+__builtin_ctz(value));
            break;
        case FIQ_HANDLED_BITFIELD_OFFSET+12:
            irq->irq_buffer[3] &= ~value;
            trace_psp_irq_clear_bit(96+__builtin_ctz(value));
            break;
        default:
            trace_psp_irq_write_unimplemented(offset, value, size);
            break;
    }

    /* Firmware ACKed one or more sources; drop the line if none remain. */
    psp_irq_update(irq);
}

static void psp_irq_write(void* opaque, hwaddr offset, uint64_t value,
                          unsigned int size)
{
    PSPIrqState* irq = PSP_IRQ(opaque);

    if (offset < 0x10) {
        psp_irq_write_ccp_queue(irq, offset, value);
    } else if (offset >= 0xb0 && offset < 0xc8) {
        psp_irq_write_fiq(irq, offset, value, size);
    } else {
        trace_psp_irq_write_unimplemented(offset, value, size);
    }
}
static uint32_t psp_irq_read_fiq(PSPIrqState* irq, hwaddr offset, unsigned int size) {
    switch (offset) {
        case FIQ_ALL_PROCESSED_OFFSET:
            //TODO: pull irq line down maybe here if returning 1 --> all cleared
            return irq->irq_buffer[0] == 0 && irq->irq_buffer[1] == 0 && irq->irq_buffer[2] == 0 && irq->irq_buffer[3] == 0;
        case FIQ_NEXT_OFFSET:
            for (int i = 0; i < 4; i++)
                if (irq->irq_buffer[i] > 0)
                    return 32*i + __builtin_ctz(irq->irq_buffer[i]);
            return 0;
        default:
            trace_psp_irq_read_unimplemented(offset, size);
            return 0;
    }
}

static uint64_t psp_irq_read(void* opaque, hwaddr offset, unsigned int size)
{
    PSPIrqState* irq = PSP_IRQ(opaque);
    uint64_t value;

    value = 0;

    if (offset < 0x10) {
        // we are inside the interrupt_bitfield
        value = irq->ccp_queue_bitfield[offset >> 2];
        trace_psp_irq_read_bitfield(offset, value, size);
    } else if (offset >= 0xb0 && offset < 0xc8) {
        value =  psp_irq_read_fiq(irq, offset, size);
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

    // clear the bitfields
    memset(&s->ccp_queue_bitfield, 0, sizeof(s->ccp_queue_bitfield));
    memset(&s->irq_buffer, 0, sizeof(s->irq_buffer));

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

    /* One GPIO input per interrupt source (4 groups x 32); devices connect
     * their output line to the matching source index. */
    qdev_init_gpio_in(dev, psp_irq_set_irq, 4 * 32);

    /* Output line to the PSP core. */
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->parent_irq);
}

/*
 * Debug hook for source-assert experiments: mark the given group-N sources
 * pending at reset (e.g. force_group0=0x20000000 raises id 0x1d). The firmware
 * clears them with the normal ACK write.
 */
static void psp_irq_reset_hold(Object* obj, ResetType type)
{
    PSPIrqState* s = PSP_IRQ(obj);

    s->irq_buffer[0] = s->force_group0;
    psp_irq_update(s);
}

static const Property properties[] = {
    DEFINE_PROP_STRING("psp_irq_ident", PSPIrqState, ident),
    DEFINE_PROP_UINT32("psp_irq_generation", PSPIrqState, generation,
                       PSP_GENERATION_MAX),
    DEFINE_PROP_UINT32("force_group0", PSPIrqState, force_group0, 0),
};

/*
 * Snapshot the full interrupt state so the fuzzer's fast-snapshot
 * (syx) restores pending/ACK state deterministically every iteration.
 */
static const VMStateDescription psp_irq_vmstate = {
    .name = "psp-irq",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(ccp_queue_bitfield, PSPIrqState, 4),
        VMSTATE_UINT32_ARRAY(irq_buffer, PSPIrqState, 4),
        VMSTATE_END_OF_LIST()
    }
};

static void psp_irq_class_init(ObjectClass* oc, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    ResettableClass* rc = RESETTABLE_CLASS(oc);
    dc->realize = psp_irq_realize;
    dc->vmsd = &psp_irq_vmstate;
    rc->phases.hold = psp_irq_reset_hold;
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
