/*
 * AMD PSP SPI controller: command mode access to the SMN attached flash
 *
 * Registers (byte offsets, inferred from the firmware's use, not from a
 * datasheet): +0x45 opcode, +0x47 trigger (0x80 starts the command, reads
 * back 0 once it is done), +0x48 number of transmit bytes, +0x4b number of
 * receive bytes, +0x4c status (bit 31 busy, never set), +0x80.. FIFO. The
 * transmit bytes are placed at the start of the FIFO, the receive bytes
 * follow them. The flash behind it is the SMN flash RAM.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"

#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "trace.h"

#include "psp/smn/psp-smn-spi.h"

#define REG_OPCODE 0x45
#define REG_TRIGGER 0x47
#define REG_TXCOUNT 0x48
#define REG_RXCOUNT 0x4b
#define REG_STATUS 0x4c
#define REG_FIFO 0x80
#define FIFO_SIZE (PSP_SMN_SPI_SIZE - REG_FIFO)
#define TRIGGER_GO 0x80

#define OP_PAGE_PROGRAM 0x02
#define OP_READ 0x03
#define OP_WRDI 0x04
#define OP_RDSR 0x05
#define OP_WREN 0x06
#define OP_ERASE_4K 0x20
#define OP_ERASE_32K 0x52
#define OP_ERASE_64K 0xd8

#define SR_WEL 0x02

static bool psp_spi_addr(PSPSmnSpiState *s, const uint8_t *tx, unsigned ntx,
                         uint64_t *addr)
{
    if (ntx < 3) {
        return false;
    }
    *addr = (tx[0] << 16) | (tx[1] << 8) | tx[2];
    return *addr < memory_region_size(s->flash);
}

static void psp_spi_command(PSPSmnSpiState *s)
{
    uint8_t op = s->regs[REG_OPCODE];
    unsigned ntx = MIN(s->regs[REG_TXCOUNT], FIFO_SIZE);
    unsigned nrx = MIN(s->regs[REG_RXCOUNT], FIFO_SIZE - ntx);
    uint8_t *tx = &s->regs[REG_FIFO];
    uint8_t *rx = tx + ntx;
    uint8_t *flash = memory_region_get_ram_ptr(s->flash);
    uint64_t size = memory_region_size(s->flash);
    uint64_t addr, len;

    trace_psp_smn_spi_command(op, ntx, nrx);
    switch (op) {
    case OP_WREN:
        s->wel = true;
        break;
    case OP_WRDI:
        s->wel = false;
        break;
    case OP_RDSR:
        memset(rx, s->wel ? SR_WEL : 0, nrx);
        break;
    case OP_READ:
        if (psp_spi_addr(s, tx, ntx, &addr)) {
            for (unsigned i = 0; i < nrx; i++) {
                rx[i] = addr + i < size ? flash[addr + i] : 0xff;
            }
        } else {
            memset(rx, 0xff, nrx);
        }
        break;
    case OP_PAGE_PROGRAM:
        /* The chip programs within a 256 byte page and wraps around in it */
        if (s->wel && psp_spi_addr(s, tx, ntx, &addr)) {
            uint64_t page = addr & ~0xffULL;

            for (unsigned i = 3; i < ntx; i++) {
                uint64_t a = page | ((addr + i - 3) & 0xff);

                flash[a] &= tx[i];
                memory_region_set_dirty(s->flash, a, 1);
            }
        }
        s->wel = false;
        break;
    case OP_ERASE_4K:
    case OP_ERASE_32K:
    case OP_ERASE_64K:
        if (s->wel && psp_spi_addr(s, tx, ntx, &addr)) {
            len = op == OP_ERASE_4K ? 0x1000 : op == OP_ERASE_32K ? 0x8000
                                                                  : 0x10000;
            addr &= ~(len - 1);
            memset(flash + addr, 0xff, MIN(len, size - addr));
            memory_region_set_dirty(s->flash, addr, MIN(len, size - addr));
        }
        s->wel = false;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "psp-spi: unsupported opcode 0x%02x\n", op);
        break;
    }
}

static uint64_t psp_spi_read(void *opaque, hwaddr offset, unsigned size)
{
    PSPSmnSpiState *s = opaque;
    uint64_t val = 0;

    for (unsigned i = 0; i < size && offset + i < PSP_SMN_SPI_SIZE; i++) {
        val |= (uint64_t)s->regs[offset + i] << (8 * i);
    }
    trace_psp_smn_spi_read(offset, val);
    return val;
}

static void psp_spi_write(void *opaque, hwaddr offset, uint64_t val,
                          unsigned size)
{
    PSPSmnSpiState *s = opaque;

    trace_psp_smn_spi_write(offset, val);
    for (unsigned i = 0; i < size && offset + i < PSP_SMN_SPI_SIZE; i++) {
        hwaddr o = offset + i;
        uint8_t b = val >> (8 * i);

        if (o == REG_STATUS + 3) {
            continue;           /* bit 31 (busy) is read-only, always clear */
        }
        s->regs[o] = b;
        if (o == REG_TRIGGER && (b & TRIGGER_GO) && s->flash) {
            psp_spi_command(s);
            s->regs[REG_TRIGGER] = 0;
        }
    }
}

static const MemoryRegionOps psp_spi_ops = {
    .read = psp_spi_read,
    .write = psp_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void psp_spi_realize(DeviceState *dev, Error **errp)
{
    PSPSmnSpiState *s = PSP_SMN_SPI(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &psp_spi_ops, s,
                          TYPE_PSP_SMN_SPI, PSP_SMN_SPI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void psp_spi_reset_hold(Object *obj, ResetType type)
{
    PSPSmnSpiState *s = PSP_SMN_SPI(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->wel = false;
}

static const VMStateDescription psp_spi_vmstate = {
    .name = TYPE_PSP_SMN_SPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, PSPSmnSpiState, PSP_SMN_SPI_SIZE),
        VMSTATE_BOOL(wel, PSPSmnSpiState),
        VMSTATE_END_OF_LIST()
    }
};

static void psp_spi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = psp_spi_realize;
    dc->vmsd = &psp_spi_vmstate;
    rc->phases.hold = psp_spi_reset_hold;
}

static const TypeInfo psp_spi_info = {
    .name = TYPE_PSP_SMN_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PSPSmnSpiState),
    .class_init = psp_spi_class_init,
};

static void psp_spi_register_types(void)
{
    type_register_static(&psp_spi_info);
}

type_init(psp_spi_register_types);
