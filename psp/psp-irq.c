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
#include "qapi/visitor.h"
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
    /* ccp_queue_bitfield[] holds the per-group enable mask (+0x0..0xc). */
    bool pending = false;
    for (int i = 0; i < 4; i++) {
        pending |= (irq->irq_buffer[i] & irq->ccp_queue_bitfield[i]) != 0;
    }
    qemu_set_irq(irq->parent_irq, pending);
    qemu_set_irq(irq->parent_fiq, pending);
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


/* +0x0..0xc: per-group enable mask, one word per group of 32 sources. */
static void psp_irq_write_enable(PSPIrqState* irq, hwaddr offset,
                                 uint64_t value)
{
    uint32_t old = irq->ccp_queue_bitfield[offset >> 2];

    trace_psp_irq_write_bitfield(offset, value, 4);
    trace_psp_irq_enable_write(offset >> 2, old, value, value & ~old,
                               old & ~value);
    irq->ccp_queue_bitfield[offset >> 2] = value;
    psp_irq_update(irq);
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
        psp_irq_write_enable(irq, offset, value);
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

/*
 * Runtime hook (qom-set only, device must be realized): raise source id
 * (group * 32 + bit) through the same GPIO input the devices use. Pending
 * state is cleared by the firmware's ACK write, as for device interrupts.
 */
static void psp_irq_set_inject(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    PSPIrqState *s = PSP_IRQ(obj);
    uint32_t id;

    if (!visit_type_uint32(v, name, &id, errp)) {
        return;
    }
    if (!qdev_is_realized(DEVICE(s))) {
        error_setg(errp, "device must be realized to inject an interrupt");
        return;
    }
    if (id >= PSP_IRQ_NUM_SOURCES) {
        error_setg(errp, "interrupt source %u out of range", id);
        return;
    }
    qemu_set_irq(qdev_get_gpio_in(DEVICE(s), id), 1);
}

/*
 * Fuzzer hook, same pattern as the aspfuzz_smn_* externs: raise source id
 * (group * 32 + bit) without a QOM lookup. The BQL is taken if the caller does
 * not hold it, because raising a line can reach cpu_interrupt(), which asserts
 * it. Returns -1 if no IC device is realized or id is out of range.
 */
static PSPIrqState* aspfuzz_irq_state;

int32_t aspfuzz_irq_inject(uint32_t id);
int32_t aspfuzz_irq_inject(uint32_t id)
{
    BQL_LOCK_GUARD();

    if (aspfuzz_irq_state == NULL || id >= PSP_IRQ_NUM_SOURCES) {
        return -1;
    }
    qemu_set_irq(qdev_get_gpio_in(DEVICE(aspfuzz_irq_state), id), 1);
    return 0;
}

static void psp_irq_init(Object* obj)
{
    PSPIrqState* s = PSP_IRQ(obj);
    s->ident = NULL;
    object_property_add(obj, "inject", "uint32", NULL, psp_irq_set_inject,
                        NULL, NULL);
}

static void psp_irq_unrealize(DeviceState *dev)
{
    if (aspfuzz_irq_state == PSP_IRQ(dev)) {
        aspfuzz_irq_state = NULL;
    }
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
    qdev_init_gpio_in(dev, psp_irq_set_irq, PSP_IRQ_NUM_SOURCES);

    /* Output line to the PSP core. */
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->parent_irq);
    /* Off-chip BL services IRQ, SecOS services FIQ; each stage masks the
     * other, so both lines follow the same pending state. */
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->parent_fiq);

    aspfuzz_irq_state = s;
}

/*
 * Debug hook for source-assert experiments: mark the given group-N sources
 * pending at reset. The firmware clears them with the normal ACK write.
 * Bit n of force_groupN is source N * 32 + n, e.g.:
 *   force_group0=0x20000000  raises id 0x1d (group0 bit 29)
 *   force_group1=0x04000000  raises id 0x3a (group1 bit 26)
 *   force_group3=0x00000001  raises id 0x60 (group3 bit 0, suspected tick)
 * Do not use group3 bits 16..20 (ids 0x70..0x73): their firmware handler
 * writes through the pointer at PSP 0x3210044, which is not modelled.
 */
static void psp_irq_reset_hold(Object* obj, ResetType type)
{
    PSPIrqState* s = PSP_IRQ(obj);

    s->irq_buffer[0] = s->force_group0;
    s->irq_buffer[1] = s->force_group1;
    s->irq_buffer[2] = 0;
    s->irq_buffer[3] = s->force_group3;
    psp_irq_update(s);
}

static const Property properties[] = {
    DEFINE_PROP_STRING("psp_irq_ident", PSPIrqState, ident),
    DEFINE_PROP_UINT32("psp_irq_generation", PSPIrqState, generation,
                       PSP_GENERATION_MAX),
    DEFINE_PROP_UINT32("force_group0", PSPIrqState, force_group0, 0),
    DEFINE_PROP_UINT32("force_group1", PSPIrqState, force_group1, 0),
    DEFINE_PROP_UINT32("force_group3", PSPIrqState, force_group3, 0),
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
    dc->unrealize = psp_irq_unrealize;
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
