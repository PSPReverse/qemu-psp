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
#include "qemu/log.h"
#include "trace.h"

#include "psp/psp.h"

static char ident[] = "PSP Timer";

/* Assumed 100 MHz counter (10 ns per count); the period is clamped to 1 us */
static void psp_timer_arm(PSPTimerState* s)
{
    int64_t period_ns;

    if (s->expiry_irq_id < 0 || !(s->psp_timer_control & 1) ||
        s->psp_timer_period == 0) {
        timer_del(&s->expiry_timer);
        return;
    }
    period_ns = MAX((int64_t)s->psp_timer_period * 10, 1000);
    timer_mod(&s->expiry_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + period_ns);
}

static void psp_timer_expire(void* opaque)
{
    PSPTimerState* s = PSP_TIMER(opaque);

    qemu_set_irq(s->expiry_irq, 1);
    psp_timer_arm(s);
}

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
    case 0x20:
        val = s->psp_timer_count;
        trace_psp_timer_counter_read(val);
        if ((s->psp_timer_control & 0x1) && lane == 0) {
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
    case 0x1:
        /* XXX For single byte access by the on chip bl... */
        s->psp_timer_control |= val << 8;
        trace_psp_timer_control_write(offset, val);
        break;
    case 0x20:
        s->psp_timer_count = val;
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
};

static void psp_timer_class_init(ObjectClass* oc, const void* data)
{
    device_class_set_props(DEVICE_CLASS(oc), psp_timer_properties);
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
