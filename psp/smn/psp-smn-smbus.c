/*
 * AMD PSP Zen3 SMBus controller with DDR4 SPD EEPROMs.
 *
 * Minimal model of the controller at SMN 0x2d01a00 the ABL uses to read the
 * DIMM SPD data. The register layout is PIIX4-like (derived from static
 * analysis of the caller, not from documentation):
 *   +0 host status      bit0 busy, bit1 done, bit2 failed (NACK), bit3 bus
 *                       error; write-1-clear
 *   +1 slave status     write-1-clear
 *   +2 control          bits 4:2 protocol (2 byte-data, 3 word-data), bit 6
 *                       START
 *   +3 command, +4 address (7-bit << 1 | read), +5/+6 data0/data1
 *   +8                  bit 4 reads back 1
 * A transaction completes instantaneously on START. The other registers
 * read/write back.
 *
 * The EEPROMs sit at 7-bit 0x50 + n behind a multiplexer at 0x70 that only
 * ACKs. The SPD page (DDR4 SPA0/SPA1 convention) is selected by an access to
 * 7-bit 0x36 (page 0) or 0x37 (page 1). The SPD image is generated
 * from the public JEDEC DDR4 SPD layout unless a custom image is given.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"

#include "psp/smn/psp-smn-smbus.h"

#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "trace.h"

#define REG_HSTS 0
#define REG_SLVSTS 1
#define REG_CNT 2
#define REG_CMD 3
#define REG_ADDR 4
#define REG_DATA0 5
#define REG_DATA1 6
#define REG_MISC 8

#define HSTS_DONE 0x02
#define HSTS_FAILED 0x04
#define HSTS_CLEARABLE 0x1f
#define SLVSTS_CLEARABLE 0x3f
#define CNT_START 0x40
#define CNT_PROTO(cnt) (((cnt) >> 2) & 7)
#define MISC_BIT4 0x10

enum {
    PROTO_QUICK = 0,
    PROTO_BYTE = 1,
    PROTO_BYTE_DATA = 2,
    PROTO_WORD_DATA = 3,
};

#define ADDR_MUX 0x70
#define ADDR_SPD_PAGE0 0x36
#define ADDR_SPD_PAGE1 0x37
#define ADDR_SPD_BASE 0x50

uint16_t psp_spd_crc16(const uint8_t* data, size_t len)
{
    uint16_t crc = 0;

    while (len--) {
        crc ^= (uint16_t)*data++ << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
        }
    }
    return crc;
}

/*
 * One valid DDR4 RDIMM image: 8 GiB, single rank, 8 x 8 Gbit x8 devices,
 * DDR4-2400 17-17-17. Time values use the 125 ps MTB with 1 ps fine offsets.
 */
static void psp_spd_generate(uint8_t* spd, unsigned slot)
{
    static const char part[20] = "QEMU-PSP-DIMM       ";
    uint16_t crc;

    memset(spd, 0, PSP_SPD_SIZE);

    spd[0] = 0x23;          /* 384 bytes used, 512 total */
    spd[1] = 0x11;          /* SPD revision 1.1 */
    spd[2] = 0x0c;          /* DDR4 SDRAM */
    spd[3] = 0x01;          /* RDIMM */
    spd[4] = 0x84;          /* 8 Gbit, 4 bank groups of 4 banks */
    spd[5] = 0x21;          /* 16 row, 10 column bits */
    spd[6] = 0x00;          /* monolithic package */
    spd[11] = 0x03;         /* 1.2 V */
    spd[12] = 0x01;         /* x8 devices, 1 rank */
    spd[13] = 0x03;         /* 64 bit bus, no ECC */
    spd[17] = 0x00;         /* MTB 125 ps, FTB 1 ps */
    spd[18] = 0x07;         /* tCKmin 0.833 ns (0.875 ns - 42 ps) */
    spd[19] = 0x0d;         /* tCKmax 1.6 ns (1.625 ns - 25 ps) */
    spd[20] = 0xa8;         /* CL 10 12 14 */
    spd[21] = 0x0f;         /* CL 15 16 17 18 */
    spd[24] = 0x71;         /* tAAmin 14.16 ns */
    spd[25] = 0x71;         /* tRCDmin */
    spd[26] = 0x71;         /* tRPmin */
    spd[27] = 0x11;         /* tRC / tRAS upper nibbles */
    spd[28] = 0x00;         /* tRASmin 32 ns */
    spd[29] = 0x71;         /* tRCmin 46.16 ns */
    spd[30] = 0xf0;         /* tRFC1min 350 ns */
    spd[31] = 0x0a;
    spd[32] = 0x20;         /* tRFC2min 260 ns */
    spd[33] = 0x08;
    spd[34] = 0x00;         /* tRFC4min 160 ns */
    spd[35] = 0x05;
    spd[36] = 0x00;         /* tFAWmin 21 ns */
    spd[37] = 0xa8;
    spd[38] = 0x1a;         /* tRRD_Smin 3.3 ns */
    spd[39] = 0x27;         /* tRRD_Lmin 4.9 ns */
    spd[40] = 0x28;         /* tCCD_Lmin 5 ns */
    spd[41] = 0x00;         /* tWRmin 15 ns */
    spd[42] = 0x78;
    spd[117] = 0x00;        /* fine offsets */
    spd[118] = 0x19;
    spd[119] = 0x32;
    spd[120] = 0x23;
    spd[121] = 0x23;
    spd[122] = 0x23;
    spd[123] = 0x23;
    spd[124] = 0xe7;
    spd[125] = 0xd6;
    crc = psp_spd_crc16(spd, 126);
    spd[126] = crc & 0xff;
    spd[127] = crc >> 8;

    /* RDIMM module specific section */
    spd[128] = 0x0f;        /* 30 mm < height <= 31 mm */
    spd[131] = 0x01;        /* register type: RDIMM register */
    spd[133] = 0x00;        /* register manufacturer, no continuation codes */
    spd[134] = 0xfe;        /* ID 0x7e with odd parity */
    crc = psp_spd_crc16(spd + 128, 126);
    spd[254] = crc & 0xff;
    spd[255] = crc >> 8;

    /* Manufacturing information */
    spd[320] = 0x00;        /* module manufacturer, no continuation codes */
    spd[321] = 0xfe;        /* ID 0x7e with odd parity */
    spd[323] = 0x24;        /* year, BCD */
    spd[324] = 0x01;        /* week, BCD */
    stl_le_p(spd + 325, 0x50530000 | slot);     /* serial number */
    memcpy(spd + 329, part, sizeof(part));
    spd[350] = 0x00;        /* DRAM manufacturer, same encoding as above */
    spd[351] = 0xfe;
}

static bool psp_smbus_spd_present(PSPSmnSmbusState* s, unsigned addr7)
{
    return addr7 >= ADDR_SPD_BASE &&
           addr7 < ADDR_SPD_BASE + PSP_SMN_SMBUS_SLOTS &&
           (s->dimm_slots & (1u << (addr7 - ADDR_SPD_BASE)));
}

static void psp_smbus_spd_access(PSPSmnSmbusState* s, unsigned slot,
                                 unsigned proto, bool read)
{
    const uint8_t* spd = s->spd[slot];
    unsigned base = s->spd_page * 256;

    switch (proto) {
    case PROTO_QUICK:
        break;
    case PROTO_BYTE:
        if (read) {
            s->regs[REG_DATA0] = spd[base + s->spd_ptr++];
        } else {
            s->spd_ptr = s->regs[REG_CMD];
        }
        break;
    case PROTO_BYTE_DATA:
    case PROTO_WORD_DATA:
        /* Writes are dropped: the EEPROM is write protected */
        if (read) {
            s->spd_ptr = s->regs[REG_CMD];
            s->regs[REG_DATA0] = spd[base + s->spd_ptr++];
            if (proto == PROTO_WORD_DATA) {
                s->regs[REG_DATA1] = spd[base + s->spd_ptr++];
            }
        }
        break;
    }
}

/* Run the transaction programmed in the registers */
static void psp_smbus_start(PSPSmnSmbusState* s)
{
    unsigned proto = CNT_PROTO(s->regs[REG_CNT]);
    unsigned addr7 = s->regs[REG_ADDR] >> 1;
    bool read = s->regs[REG_ADDR] & 1;
    bool ack = true;

    if (proto > PROTO_WORD_DATA) {
        /* Block and process call transfers are not modelled (inferred) */
        ack = false;
    } else if (psp_smbus_spd_present(s, addr7)) {
        psp_smbus_spd_access(s, addr7 - ADDR_SPD_BASE, proto, read);
    } else if (addr7 == ADDR_SPD_PAGE0 || addr7 == ADDR_SPD_PAGE1) {
        s->spd_page = addr7 & 1;
    } else if (addr7 != ADDR_MUX) {
        ack = false;
    }
    trace_psp_smn_smbus_xfer(addr7, read, proto, s->regs[REG_CMD], ack);
    s->regs[REG_HSTS] |= ack ? HSTS_DONE : HSTS_FAILED;
}

static uint64_t psp_smbus_read(void* opaque, hwaddr offset, unsigned size)
{
    PSPSmnSmbusState* s = opaque;
    uint8_t val = s->regs[offset];

    if (offset == REG_MISC) {
        val |= MISC_BIT4;
    }
    trace_psp_smn_smbus_read(offset, val);
    return val;
}

static void psp_smbus_write(void* opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    PSPSmnSmbusState* s = opaque;
    uint8_t val = value;

    trace_psp_smn_smbus_write(offset, val);
    switch (offset) {
    case REG_HSTS:
        s->regs[REG_HSTS] &= ~(val & HSTS_CLEARABLE);
        break;
    case REG_SLVSTS:
        s->regs[REG_SLVSTS] &= ~(val & SLVSTS_CLEARABLE);
        break;
    case REG_CNT:
        /* START is not stored */
        s->regs[REG_CNT] = val & ~CNT_START;
        if (val & CNT_START) {
            psp_smbus_start(s);
        }
        break;
    default:
        s->regs[offset] = val;
        break;
    }
}

static const MemoryRegionOps psp_smbus_ops = {
    .read = psp_smbus_read,
    .write = psp_smbus_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static void psp_smbus_realize(DeviceState* dev, Error** errp)
{
    PSPSmnSmbusState* s = PSP_SMN_SMBUS(dev);
    g_autofree char* image = NULL;
    gsize len;

    if (s->dimm_slots >> PSP_SMN_SMBUS_SLOTS) {
        error_setg(errp, "dimm-slots: only %d slots", PSP_SMN_SMBUS_SLOTS);
        return;
    }
    if (s->spd_file) {
        GError* err = NULL;

        if (!g_file_get_contents(s->spd_file, &image, &len, &err)) {
            error_setg(errp, "spd-file: %s", err->message);
            g_error_free(err);
            return;
        }
        if (len != PSP_SPD_SIZE) {
            error_setg(errp, "spd-file: %s is not %d bytes", s->spd_file,
                       PSP_SPD_SIZE);
            return;
        }
    }
    for (unsigned i = 0; i < PSP_SMN_SMBUS_SLOTS; i++) {
        if (image) {
            memcpy(s->spd[i], image, PSP_SPD_SIZE);
        } else {
            psp_spd_generate(s->spd[i], i);
        }
    }
    memory_region_init_io(&s->iomem, OBJECT(dev), &psp_smbus_ops, s,
                          "psp-smn-smbus", PSP_SMN_SMBUS_REGS);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void psp_smbus_reset_hold(Object* obj, ResetType type)
{
    PSPSmnSmbusState* s = PSP_SMN_SMBUS(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->spd_page = 0;
    s->spd_ptr = 0;
}

static const VMStateDescription psp_smbus_vmstate = {
    .name = "psp-smn-smbus",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, PSPSmnSmbusState, PSP_SMN_SMBUS_REGS),
        VMSTATE_UINT8(spd_page, PSPSmnSmbusState),
        VMSTATE_UINT8(spd_ptr, PSPSmnSmbusState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property psp_smbus_properties[] = {
    DEFINE_PROP_UINT32("dimm-slots", PSPSmnSmbusState, dimm_slots, 2),
    DEFINE_PROP_STRING("spd-file", PSPSmnSmbusState, spd_file),
};

static void psp_smbus_class_init(ObjectClass* oc, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    ResettableClass* rc = RESETTABLE_CLASS(oc);

    dc->realize = psp_smbus_realize;
    dc->vmsd = &psp_smbus_vmstate;
    rc->phases.hold = psp_smbus_reset_hold;
    device_class_set_props(dc, psp_smbus_properties);
}

static const TypeInfo psp_smbus_info = {
    .name = TYPE_PSP_SMN_SMBUS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PSPSmnSmbusState),
    .class_init = psp_smbus_class_init,
};

static void psp_smbus_register_types(void)
{
    type_register_static(&psp_smbus_info);
}

type_init(psp_smbus_register_types)
