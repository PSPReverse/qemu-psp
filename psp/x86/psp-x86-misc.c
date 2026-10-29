/*
 * Copyright (C) 2024 Stefan Zabka <stefan@zabka.it>
 * SPDX-License-Identifier: GPL-2.0
 */
#include "qemu/osdep.h"

#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "trace.h"

#include "psp/x86/x86-misc.h"
#include "psp/psp-regs.h"

typedef struct RegionInfo {
    char const* const region_name;
    uint64_t const base;
    uint64_t const start;
    uint64_t const end;
} RegionInfo;

/**
 * @brief region mapping as defined in the PPR NRB
 *
 * The current interpretation of the table is that
 * FEDC0000: I2Cx2000...x60FC
 * FEDC0000: UARTx9000...xA0FC
 * FEDC0000: I2CxB000...xB0FC
 * FEDC0000: UARTxE000...xF0FC
 *
 * Means that all of these have a shared base reagion of FEDC0000
 * and the offsets are relative to that base, but that doesn't
 * work for the PM region
 * FED80300: PMx00000000...x0000005F
 * FED80300: PMx5F_x00..._x14
 * FED80300: PMx00000060...x000000FC
 *
 * Where the two PMx regions don't leave any space for the PMx5F region
 * As such, another interpretation would be that within a given base
 * all regions are continous so that after the 0x5F bytes of PM region
 * there is 0x14 bytes of PMx5F region and then the rest of the PM
 * region
 */
static const RegionInfo regions[] = {
    {"IOAPIC", 0xFEC00000, 0x0, 0x10},
    // TODO: Figure out this line
    // FEC00000:IOAPICx00000010_indirectaddressoffset00...x00000010_indirectaddressoffset3F
    {"IOAPIC", 0xFEC00000, 0x20, 0x40},
    {"SPI", 0xFEC10000, 0x0, 0xFC},
    {"ESPI", 0xFEC20000, 0x0, 0xAC},
    {"HPET", 0xFED00000, 0x0, 0x1EC},
    {"SMBUSPCI", 0xFED80000, 0x0, 0xFC},
    {"SMI", 0xFED80200, 0x0, 0xC4},
    {"PM", 0xFED80300, 0x0, 0x110}, // See the confusion above 0x110 = 0xFC
                                    // (length of PM) + 0x14 (length of PMx5F)
    {"PM2", 0xFED80400, 0x0, 0xFF},
    {"RTCHOST", 0xFED80700, 0x0, 0x7F},
    {"PM", 0xFED80300, 0x500, 0x528},
    {"ASF", 0xFED80900, 0x0, 0x17},
    {"SMBUS", 0xFED80A00, 0x0, 0x17},
    {"WDT", 0xFED80B00, 0x0, 0x04},
    {"IOMUX", 0xFED80D00, 0x0, 0x98},
    {"MISC", 0xFED80E00, 0x0, 0xFC},
    {"REMOTEGPIO", 0xFED81200, 0x0, 0xFC},
    {"MISC2", 0xFED81300, 0x0, 0xBC},
    {"GPIO", 0xFED81500, 0x0, 0x37C},
    {"ACDC", 0xFED81D00, 0x0, 0x20},
    {"AOAC", 0xFED81E00, 0x0, 0xFC},
    {"I2C", 0xFEDC0000, 0x2000, 0x60FC},
    {"UART", 0xFEDC0000, 0x9000, 0xA0FC},
    {"I2C", 0xFEDC0000, 0xB000, 0xB0FC},
    {"UART", 0xFEDC0000, 0xE000, 0xF0FC},
    {"EMMCHC", 0xFEDD5000, 0x0, 0xFC},
    {"EMMCCFG", 0xFEDD5800, 0x0, 0x10C},
    {"LEGACYIO", 0xFF000000, 0x0, 0xCF9},
    {"unknown", 0x0, 0x0, 0x0} // sentinel
};
typedef struct LoggingInfo {
    char const* const region_name;
    uint64_t const offset;
} LoggingInfo;
LoggingInfo get_logging_info_for_addr(hwaddr addr);
LoggingInfo get_logging_info_for_addr(hwaddr addr)
{
    const RegionInfo* regions_iter = regions;
    while (regions_iter->base != 0) {
        uint64_t base = regions_iter->base;
        uint64_t start = base + regions_iter->start;
        uint64_t end = base + regions_iter->end;
        if (addr >= start && addr <= end) {
            return (LoggingInfo){regions_iter->region_name, addr - base};
        }
        regions_iter++;
    }
    return (LoggingInfo){"unknown", addr};
}

static uint64_t psp_x86_misc_read(void* opaque, hwaddr addr, unsigned int size)
{
    PSPX86MiscState *s = PSP_X86_MISC(opaque);
    uint64_t val;
    LoggingInfo info = get_logging_info_for_addr(addr);

    if (addr == 0xfffdfc000061) {
        /*
         * I/O port 0x61 (NMI status/control) through the 0xfffdfc000000
         * window. Placeholder following the PC-compatible port 0x61
         * convention; inferred, not taken from the PPR. Bits 0-1 read back
         * the last write. Bit 4 is the refresh toggle: it flips on every
         * read, a deterministic stand-in for the ~15 us refresh clock. Bit 5
         * (timer-2 output) is stubbed to 0. Applied to all generations as
         * chipset behaviour.
         */
        val = (s->port61_ctl & 0x3) | (s->port61_refresh ? 0x10 : 0);
        s->port61_refresh = !s->port61_refresh;
    } else if (s->generation == ZEN3 && (addr >> 24) == 0xfffdf7 &&
               (addr & 0x7fff) == 0x7f94) {
        /*
         * Per-core status byte of the 0xfffdf7xxxxxx core register alias
         * (one byte per register bit, byte +4 is bit 0). The SEV INIT
         * hardware check wants it set for every core the BL lists as
         * present. Inferred from that check; every core reads as ready.
         */
        val = 1;
    } else if (!psp_regs_lookup(s->generation, PSP_REG_SPACE_X86_MISC, addr,
                                &val)) {
        trace_psp_x86_misc_read_unimplemented(addr, info.region_name,
                                              info.offset);
        return 0;
    }

    trace_psp_x86_misc_read(addr, val, info.region_name, info.offset);
    return val;
}

static void psp_x86_misc_write(void* opaque, hwaddr addr, uint64_t value,
                               unsigned int size)
{
    PSPX86MiscState* s = PSP_X86_MISC(opaque);
    LoggingInfo info = get_logging_info_for_addr(addr);

    /* Port 0x61 placeholder: bits 0-1 stored, see psp_x86_misc_read */
    if (addr == 0xfffdfc000061) {
        s->port61_ctl = value & 0x3;
        trace_psp_x86_misc_write(addr, value, info.region_name, info.offset);
        return;
    }
    /*
     * COM4 line control register (0x2e8 + 3), written by the Zen3 off-chip
     * BL. Accepted and logged but not stored; reads stay unmodelled.
     */
    if (addr == 0xfffdfc0002eb && s->generation == ZEN3) {
        trace_psp_x86_misc_write(addr, value, info.region_name, info.offset);
        return;
    }
    trace_psp_x86_misc_write_unimplemented(addr, value, info.region_name,
                                           info.offset);
}

static const MemoryRegionOps misc_mem_ops = {
    .read = psp_x86_misc_read,
    .write = psp_x86_misc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static void psp_x86_misc_realize(DeviceState* dev, Error** errp)
{
    PSPX86MiscState* s = PSP_X86_MISC(dev);
    if (s->generation >= PSP_GENERATION_MAX) {
        error_setg(errp,
                   "property 'psp_generation' not specified or invalid for PSP X86 misc");
        return;
    }
    /* Init mmio */
    memory_region_init_io(&s->iomem, OBJECT(dev), &misc_mem_ops, s,
                          "psp-x86-misc", UINT64_MAX);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription psp_x86_misc_vmstate = {
    .name = TYPE_PSP_X86_MISC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(port61_ctl, PSPX86MiscState),
        VMSTATE_BOOL(port61_refresh, PSPX86MiscState),
        VMSTATE_END_OF_LIST()
    }
};

static void psp_x86_misc_reset_hold(Object* obj, ResetType type)
{
    PSPX86MiscState* s = PSP_X86_MISC(obj);

    s->port61_ctl = 0;
    s->port61_refresh = false;
}

static const Property properties[] = {
    DEFINE_PROP_UINT32("psp_generation", PSPX86MiscState, generation,
                       PSP_GENERATION_MAX),
};

static void psp_x86_misc_class_init(ObjectClass* oc, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    ResettableClass* rc = RESETTABLE_CLASS(oc);
    dc->realize = psp_x86_misc_realize;
    dc->vmsd = &psp_x86_misc_vmstate;
    rc->phases.hold = psp_x86_misc_reset_hold;
    device_class_set_props(dc, properties);
}

static const TypeInfo psp_x86_misc_info = {
    .name = TYPE_PSP_X86_MISC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PSPX86MiscState),
    .class_init = psp_x86_misc_class_init,
};

static void psp_misc_register_types(void)
{
    type_register_static(&psp_x86_misc_info);
}

type_init(psp_misc_register_types);
