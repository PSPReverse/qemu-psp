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
 */
#include "qemu/osdep.h"

#include "psp/mmio/psp-timer.h"

#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "migration/vmstate.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "trace.h"

#include "psp/psp.h"

static char ident[] = "PSP Timer";

/*
 * Shortest expiry interval. A guest period of 1 would otherwise give a
 * 10 ns timer and an interrupt storm.
 */
#define PSP_TIMER_MIN_EXPIRY_NS 1000

/* 100 MHz counter: 10 ns per count */
static void psp_timer_arm(PSPTimerState* s)
{
    int64_t delay_ns;

    if (s->expiry_irq_id < 0 || !(s->psp_timer_control & 1) ||
        s->psp_timer_period == 0) {
        timer_del(&s->expiry_timer);
        return;
    }
    delay_ns = MAX((int64_t)s->psp_timer_period * 10,
                   PSP_TIMER_MIN_EXPIRY_NS);
    timer_mod(&s->expiry_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns);
}

static void psp_timer_expire(void* opaque)
{
    PSPTimerState* s = PSP_TIMER(opaque);

    qemu_set_irq(s->expiry_irq, 1);
    psp_timer_arm(s);
}

static void psp_timer_reset_hold(Object *obj, ResetType type)
{
    PSPTimerState *s = PSP_TIMER(obj);

    s->psp_timer_control = 0;
    s->psp_timer_count = 0;
    s->psp_timer_period = 0;
    memset(s->psp_timer_misc, 0, sizeof(s->psp_timer_misc));
    memset(s->psp_timer_placeholder, 0, sizeof(s->psp_timer_placeholder));
    s->counter_anchor_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->counter_offset_ns = 0;
    timer_del(&s->expiry_timer);
}

/*
 * Counter for counter_hz != 0: the last written value plus elapsed virtual
 * time (plus any aspfuzz_timer_advance() offset) scaled to counter_hz. The
 * enable bit is not consulted in this mode.
 */
static uint32_t psp_timer_derived_count(PSPTimerState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    /* The anchor can be ahead of the clock after a restore; do not wrap */
    uint64_t elapsed = MAX(now - s->counter_anchor_ns, 0);

    return s->psp_timer_count +
           muldiv64(elapsed + s->counter_offset_ns, s->counter_hz,
                    NANOSECONDS_PER_SECOND);
}

/*
 * Fuzzer hook, same pattern as aspfuzz_irq_inject: the realized timer1 (the
 * first instance realized; psp.c realizes timer1 before timer2). Cleared at
 * unrealize.
 */
static PSPTimerState *aspfuzz_timer_state;

/*
 * Advance the guest-visible time of the derived counter by ns. Only has an
 * effect when counter_hz != 0; with counter_hz == 0 the offset is stored but
 * not read. The opt-in expiry timer is neither fired nor re-armed. The BQL is
 * taken if the caller does not hold it. Returns -1 if timer1 is not realized
 * or the offset would overflow; else 0.
 */
int32_t aspfuzz_timer_advance(uint64_t ns)
{
    bool took_bql = !bql_locked();
    int32_t rc = -1;

    if (took_bql) {
        bql_lock();
    }
    if (aspfuzz_timer_state != NULL &&
        ns <= UINT64_MAX - aspfuzz_timer_state->counter_offset_ns) {
        aspfuzz_timer_state->counter_offset_ns += ns;
        rc = 0;
    }
    if (took_bql) {
        bql_unlock();
    }
    return rc;
}

static void psp_timer_realize(DeviceState *dev, Error **errp)
{
    if (aspfuzz_timer_state == NULL) {
        aspfuzz_timer_state = PSP_TIMER(dev);
    }
}

static void psp_timer_unrealize(DeviceState *dev)
{
    if (aspfuzz_timer_state == PSP_TIMER(dev)) {
        aspfuzz_timer_state = NULL;
    }
}

/*
 * Only the opt-in expiry timer is re-armed. The new deadline is one period
 * from the restore point, not from the instant the state was saved, so the
 * expiry phase is not preserved across a snapshot.
 */
static int psp_timer_post_load(void *opaque, int version_id)
{
    PSPTimerState *s = PSP_TIMER(opaque);

    if (s->expiry_irq_id >= 0) {
        psp_timer_arm(s);
    }
    return 0;
}

static const VMStateDescription psp_timer_vmstate = {
    .name = TYPE_PSP_TIMER,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = psp_timer_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(psp_timer_control, PSPTimerState),
        VMSTATE_UINT32(psp_timer_count, PSPTimerState),
        VMSTATE_UINT32(psp_timer_period, PSPTimerState),
        VMSTATE_UINT32_ARRAY(psp_timer_misc, PSPTimerState, 4),
        VMSTATE_UINT32_ARRAY(psp_timer_placeholder, PSPTimerState, 3),
        VMSTATE_INT64(counter_anchor_ns, PSPTimerState),
        VMSTATE_UINT64(counter_offset_ns, PSPTimerState),
        VMSTATE_END_OF_LIST()
    }
};

static uint64_t psp_timer_read(void* opaque, hwaddr offset, unsigned int size)
{
    PSPTimerState* s = PSP_TIMER(opaque);
    uint64_t val;
    hwaddr phys_base = s->psp_timer_iomem.addr;

    if (size != sizeof(uint32_t) && size != sizeof(uint16_t) &&
        size != sizeof(uint8_t)) {
        qemu_log_mask(
            LOG_UNIMP,
            "%s: Error. Unsupported read size at offset 0x%" HWADDR_PRIx "\n",
            ident, phys_base + offset);
        return 0;
    }

    /* Sub-word reads return the byte lanes of the containing 32-bit register */
    hwaddr reg = offset & ~3ULL;
    unsigned lane = (offset & 3) * 8;

    // TODO do this properly instead of counting up
    switch (reg) {
    case 0:
        val = s->psp_timer_control;
        trace_psp_timer_control_read(offset, val);
        break;
    case 0x4:
    case 0x8:
    case 0xc:
        val = s->psp_timer_misc[(reg - 4) >> 2];
        break;
    case 0x10:
        val = s->psp_timer_period;
        break;
    /* Placeholder: plain read-back, meaning inferred and not modelled */
    case 0x14:
    case 0x18:
    case 0x1c:
        val = s->psp_timer_placeholder[(reg - 0x14) >> 2];
        break;
    case 0x20:
        if (s->counter_hz) {
            val = psp_timer_derived_count(s);
        } else {
            val = s->psp_timer_count;
        }
        trace_psp_timer_counter_read(val);
        if (!s->counter_hz && (s->psp_timer_control & 0x1) && lane == 0) {
            s->psp_timer_count++;
        }
        break;
    default:
        val = 0;
        break;
    }
    val >>= lane;
    if (size < sizeof(uint32_t)) {
        val &= (1ULL << (size * 8)) - 1;
    }

    return val;
}

static void psp_timer_write(void* opaque, hwaddr offset, uint64_t val,
                            unsigned int size)
{
    PSPTimerState* s = PSP_TIMER(opaque);
    hwaddr phys = s->psp_timer_iomem.addr;
    phys += offset;

    if (size != sizeof(uint32_t) && size != sizeof(uint8_t)) {
        qemu_log_mask(LOG_UNIMP,
                      "PSP Timer: Error. Unsupported write size (%d) at offset "
                      "0x%" HWADDR_PRIx " with val 0x%lx\n",
                      size, phys, val);
        return;
    }
    switch (offset) {
    case 0x0:
        s->psp_timer_control = val;
        trace_psp_timer_control_write(offset, val);
        psp_timer_arm(s);
        break;
    case 0x4:
    case 0x8:
    case 0xc:
        /* Written by the off-chip BL timer start; meaning not modelled */
        s->psp_timer_misc[(offset - 4) >> 2] = val;
        break;
    case 0x10:
        s->psp_timer_period = val;
        psp_timer_arm(s);
        break;
    /* Placeholder: plain write-back, meaning inferred and not modelled */
    case 0x14:
    case 0x18:
    case 0x1c:
        s->psp_timer_placeholder[(offset - 0x14) >> 2] = val;
        break;
    case 0x1:
        /* XXX For single byte access by the on chip bl... */
        s->psp_timer_control |= val << 8;
        trace_psp_timer_control_write(offset, val);
        break;
    case 0x20:
        s->psp_timer_count = val;
        s->counter_anchor_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        trace_psp_timer_counter_write(val);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "PSP Timer. Error: Unsupported MMIO access. "
                      "offset=0x%" HWADDR_PRIx " value=0x%lx\n",
                      offset, val);
        break;
    }
}

static const MemoryRegionOps timer_mem_ops = {
    .read = psp_timer_read,
    .write = psp_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void psp_timer_init(Object* obj)
{
    PSPTimerState* s = PSP_TIMER(obj);
    SysBusDevice* sbd = SYS_BUS_DEVICE(obj);
    s->psp_timer_control = 0;
    s->psp_timer_count = 0;

    memory_region_init_io(&s->psp_timer_iomem, obj, &timer_mem_ops, s,
                          TYPE_PSP_TIMER, PSP_TIMER_SIZE);
    sysbus_init_mmio(sbd, &s->psp_timer_iomem);
    sysbus_init_irq(sbd, &s->expiry_irq);
    timer_init_ns(&s->expiry_timer, QEMU_CLOCK_VIRTUAL, psp_timer_expire, s);
}

static const Property psp_timer_properties[] = {
    DEFINE_PROP_INT32("expiry_irq_id", PSPTimerState, expiry_irq_id, -1),
    DEFINE_PROP_UINT32("counter_hz", PSPTimerState, counter_hz, 0),
};

static void psp_timer_class_init(ObjectClass* oc, const void* data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    device_class_set_props(dc, psp_timer_properties);
    dc->realize = psp_timer_realize;
    dc->unrealize = psp_timer_unrealize;
    dc->vmsd = &psp_timer_vmstate;
    rc->phases.hold = psp_timer_reset_hold;
}

static const TypeInfo psp_timer_info = {
    .name = TYPE_PSP_TIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_init = psp_timer_init,
    .class_init = psp_timer_class_init,
    .instance_size = sizeof(PSPTimerState),
};

static void psp_timer_register_types(void)
{
    type_register_static(&psp_timer_info);
}
type_init(psp_timer_register_types);
