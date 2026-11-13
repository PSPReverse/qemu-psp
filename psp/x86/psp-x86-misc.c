/*
 * Copyright (C) 2024 Stefan Zabka <stefan@zabka.it>
 * SPDX-License-Identifier: GPL-2.0
 */
#include "qemu/osdep.h"

#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
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

/*
 * Status bytes of the 0xfffd_xxxx_xxxx core register aliases (one byte per
 * register bit): +0x7f94 is the per-core "ready" byte the SEV INIT hardware
 * check wants set for every core the BL lists as present, +0x7f90 and +0x7f91
 * are the two bytes of the per-CCX cache flush status the SEV DF_FLUSH
 * command wants set (either flush mode passes with both). All three read 1,
 * every other byte of these registers reads 0. Inferred from the applet's
 * checks, not from documentation; the aperture is matched by its low 15 bits
 * only, so every core and CCX answers the same.
 */
static bool zen3_core_alias_byte(hwaddr addr, uint8_t *byte)
{
    switch (addr & 0x7fff) {
    case 0x7f90:
    case 0x7f91:
    case 0x7f94:
        *byte = 1;
        return true;
    default:
        return false;
    }
}

static bool zen3_core_alias_read(hwaddr addr, unsigned size, uint64_t *val)
{
    bool hit = false;

    *val = 0;
    for (unsigned i = 0; i < size; i++) {
        uint8_t b;

        if (zen3_core_alias_byte(addr + i, &b)) {
            *val |= (uint64_t)b << (8 * i);
            hit = true;
        }
    }
    return hit;
}

/*
 * Memory-encryption alias. The SEV debug decrypt and encrypt handlers keep
 * the C-bit and the ASID in the high bits of a physical address (bits 43 to
 * 51 here); the memory controller strips them and, for a key, en- or decrypts
 * on the way. Nothing here is encrypted, so the alias behaves as an identity
 * key: an access that reaches this catch-all with one of those bits set is
 * forwarded to the same address with the bits cleared. The 0xfffd window of
 * the FCH never has them set legitimately and is left alone.
 */
#define X86_ENC_ALIAS_MASK 0x000ff80000000000ULL

static bool psp_x86_misc_alias(PSPX86MiscState *s, hwaddr addr, hwaddr *plain)
{
    if (s->as == NULL || !(addr & X86_ENC_ALIAS_MASK) ||
        (addr >> 32) == 0xfffd) {
        return false;
    }
    *plain = addr & ~X86_ENC_ALIAS_MASK;
    return true;
}

static uint64_t psp_x86_misc_read(void* opaque, hwaddr addr, unsigned int size)
{
    PSPX86MiscState *s = PSP_X86_MISC(opaque);
    uint64_t val;
    LoggingInfo info = get_logging_info_for_addr(addr);
    hwaddr plain;

    if (psp_x86_misc_alias(s, addr, &plain)) {
        uint8_t buf[8] = {0};

        if (address_space_read(s->as, plain, MEMTXATTRS_UNSPECIFIED, buf,
                               size) == MEMTX_OK) {
            val = ldq_le_p(buf) & MAKE_64BIT_MASK(0, size * 8);
            if (s->log_access) {
                qemu_log_mask(LOG_UNIMP, "psp-x86-misc read 0x%" HWADDR_PRIx
                              " = 0x%" PRIx64 " size %u (alias of 0x%"
                              HWADDR_PRIx ")\n", addr, val, size, plain);
            }
            return val;
        }
    }

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
    } else if (s->generation == ZEN3 && (addr >> 32) == 0xfffd &&
               (addr >> 24) != 0xfffdfc &&
               zen3_core_alias_read(addr, size, &val)) {
        /* Per-core and per-CCX status bytes, see zen3_core_alias_byte() */
    } else if (!psp_regs_lookup(s->generation, PSP_REG_SPACE_X86_MISC, addr,
                                &val)) {
        trace_psp_x86_misc_read_unimplemented(addr, info.region_name,
                                              info.offset);
        if (s->log_access) {
            qemu_log_mask(LOG_UNIMP, "psp-x86-misc read 0x%" HWADDR_PRIx
                          " = 0x0 size %u (unimplemented)\n", addr, size);
        }
        return 0;
    }

    trace_psp_x86_misc_read(addr, val, info.region_name, info.offset);
    if (s->log_access) {
        qemu_log_mask(LOG_UNIMP, "psp-x86-misc read 0x%" HWADDR_PRIx
                      " = 0x%" PRIx64 " size %u\n", addr, val, size);
    }
    return val;
}

static void psp_x86_misc_write(void* opaque, hwaddr addr, uint64_t value,
                               unsigned int size)
{
    PSPX86MiscState* s = PSP_X86_MISC(opaque);
    LoggingInfo info = get_logging_info_for_addr(addr);
    hwaddr plain;

    if (psp_x86_misc_alias(s, addr, &plain)) {
        uint8_t buf[8];

        stq_le_p(buf, value);
        if (s->log_access) {
            qemu_log_mask(LOG_UNIMP, "psp-x86-misc write 0x%" HWADDR_PRIx
                          " = 0x%" PRIx64 " size %u (alias of 0x%"
                          HWADDR_PRIx ")\n", addr, value, size, plain);
        }
        if (address_space_write(s->as, plain, MEMTXATTRS_UNSPECIFIED, buf,
                                size) == MEMTX_OK) {
            return;
        }
    }
    if (s->log_access) {
        qemu_log_mask(LOG_UNIMP, "psp-x86-misc write 0x%" HWADDR_PRIx
                      " = 0x%" PRIx64 " size %u\n", addr, value, size);
    }
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
    DEFINE_PROP_BOOL("log_access", PSPX86MiscState, log_access, false),
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
