/*
 * AMD PSP Zen3 non-volatile store, served by the x86 host.
 *
 * The Zen3 off-chip BL keeps the SEV non-volatile data in an 8 x 4 KiB block
 * device that the x86 host (SMM) provides. The BL writes a request into a
 * shared buffer in x86 RAM (the data region handed over with host mailbox
 * command 0x02) and sets bit 25 of the FCH register at 0xfed80298 to raise
 * an SMI. Here the SMI is answered synchronously by the register write.
 *
 * Shared buffer, little endian:
 *   +0x00 command; the host clears it when done (0x83 info, 0x84 read,
 *         0x85 write, 0x86 erase)
 *   +0x04 bit 31 idle/done, bit 8 request flag, bits 7:0 byte sum of the
 *         bytes from +0x08 over the size at +0x08
 *   +0x08 size of the body in bytes
 *   +0x0c error word written by the host, 0 is success
 *   +0x10 device (2), +0x14 zero
 *   +0x18 block number, +0x20 offset in block, +0x28 length
 *         (info replies block size at +0x20 and block count at +0x28;
 *         erase passes the block count at +0x20)
 *   +0x30 payload
 *
 * The register at 0xfed80298 reads bit 28 set and bit 31 clear and the one
 * at 0xfed802a8 reads bits 3:2 = 1, as the BL's readiness check wants. The
 * buffer address is a property because the registration lives in the BL.
 * With the device disabled both registers read 0 and writes are ignored.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"

#include "psp/x86/x86-nv.h"

#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "migration/vmstate.h"
#include "qemu/bswap.h"
#include "qapi/error.h"

#define TRIGGER_READY 0x10000000        /* bit 28 */
#define TRIGGER_SMI (1u << 25)
#define STATUS_READY 0x4                /* bits 3:2 = 1 */

#define CMD_INFO 0x83
#define CMD_READ 0x84
#define CMD_WRITE 0x85
#define CMD_ERASE 0x86

#define BUF_CMD 0x00
#define BUF_FLAGS 0x04
#define BUF_SIZE 0x08
#define BUF_ERROR 0x0c
#define BUF_LBA 0x18
#define BUF_OFFSET 0x20
#define BUF_LENGTH 0x28
#define BUF_PAYLOAD 0x30
#define BUF_MAX (BUF_PAYLOAD + PSP_X86_NV_BLOCK_SIZE)
#define FLAG_DONE 0x80000000u

#define NV_BYTES (PSP_X86_NV_BLOCK_SIZE * PSP_X86_NV_BLOCKS)

static uint8_t byte_sum(const uint8_t *p, uint32_t n)
{
    uint8_t sum = 0;

    while (n--) {
        sum += *p++;
    }
    return sum;
}

static bool nv_range(uint64_t lba, uint64_t off, uint64_t len, uint64_t *pos)
{
    if (lba >= PSP_X86_NV_BLOCKS || off >= PSP_X86_NV_BLOCK_SIZE ||
        len > NV_BYTES) {
        return false;
    }
    *pos = lba * PSP_X86_NV_BLOCK_SIZE + off;
    return *pos + len <= NV_BYTES;
}

/* Serve the request in the shared buffer; returns the host error word */
static uint32_t nv_execute(PSPX86NvState *s, uint8_t *b, uint32_t cmd,
                           uint32_t size)
{
    uint64_t lba = ldq_le_p(b + BUF_LBA);
    uint64_t off = ldq_le_p(b + BUF_OFFSET);
    uint64_t len = ldq_le_p(b + BUF_LENGTH);
    uint64_t pos;

    switch (cmd) {
    case CMD_INFO:
        stq_le_p(b + BUF_OFFSET, PSP_X86_NV_BLOCK_SIZE);
        stq_le_p(b + BUF_LENGTH, PSP_X86_NV_BLOCKS);
        return 0;
    case CMD_READ:
        if (!nv_range(lba, off, len, &pos) || len > PSP_X86_NV_BLOCK_SIZE ||
            BUF_PAYLOAD + len > BUF_MAX) {
            return 1;
        }
        memcpy(b + BUF_PAYLOAD, s->store + pos, len);
        return 0;
    case CMD_WRITE:
        if (!nv_range(lba, off, len, &pos) || len > PSP_X86_NV_BLOCK_SIZE ||
            BUF_PAYLOAD + len > size + BUF_SIZE) {
            return 1;
        }
        memcpy(s->store + pos, b + BUF_PAYLOAD, len);
        return 0;
    case CMD_ERASE:
        /* off carries the block count */
        if (lba >= PSP_X86_NV_BLOCKS || off > PSP_X86_NV_BLOCKS - lba) {
            return 1;
        }
        memset(s->store + lba * PSP_X86_NV_BLOCK_SIZE, 0xff,
               off * PSP_X86_NV_BLOCK_SIZE);
        return 0;
    default:
        return 1;
    }
}

static void nv_smi(PSPX86NvState *s)
{
    uint8_t b[BUF_MAX] = {};
    uint32_t cmd, flags, size, err;

    if (address_space_read(s->as, s->buffer, MEMTXATTRS_UNSPECIFIED, b,
                           BUF_PAYLOAD) != MEMTX_OK) {
        return;
    }
    cmd = ldl_le_p(b + BUF_CMD);
    flags = ldl_le_p(b + BUF_FLAGS);
    size = ldl_le_p(b + BUF_SIZE);
    if (cmd == 0 || size < 0x20 || size > BUF_MAX - BUF_SIZE ||
        address_space_read(s->as, s->buffer, MEMTXATTRS_UNSPECIFIED, b,
                           size + BUF_SIZE) != MEMTX_OK) {
        return;
    }
    if ((flags & 0xff) != byte_sum(b + BUF_SIZE, size)) {
        err = 1;
    } else {
        err = nv_execute(s, b, cmd, size);
    }
    stl_le_p(b + BUF_ERROR, err);
    stl_le_p(b + BUF_CMD, 0);
    stl_le_p(b + BUF_FLAGS, FLAG_DONE | byte_sum(b + BUF_SIZE, size));
    address_space_write(s->as, s->buffer, MEMTXATTRS_UNSPECIFIED, b,
                        size + BUF_SIZE);
}

static uint64_t nv_trigger_read(void *opaque, hwaddr offset, unsigned size)
{
    PSPX86NvState *s = opaque;

    return s->enabled ? TRIGGER_READY : 0;
}

static void nv_trigger_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    PSPX86NvState *s = opaque;

    if (s->enabled && (value & TRIGGER_SMI)) {
        nv_smi(s);
    }
}

static uint64_t nv_status_read(void *opaque, hwaddr offset, unsigned size)
{
    PSPX86NvState *s = opaque;

    return s->enabled ? STATUS_READY : 0;
}

static void nv_status_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
}

static const MemoryRegionOps nv_trigger_ops = {
    .read = nv_trigger_read,
    .write = nv_trigger_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static const MemoryRegionOps nv_status_ops = {
    .read = nv_status_read,
    .write = nv_status_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void psp_x86_nv_realize(DeviceState *dev, Error **errp)
{
    PSPX86NvState *s = PSP_X86_NV(dev);

    if (!s->as) {
        error_setg(errp, "x86 address space not set for the NV store");
        return;
    }
    memory_region_init_io(&s->trigger, OBJECT(dev), &nv_trigger_ops, s,
                          "psp-x86-nv-trigger", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->trigger);
    memory_region_init_io(&s->status, OBJECT(dev), &nv_status_ops, s,
                          "psp-x86-nv-status", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->status);
}

static void psp_x86_nv_reset_hold(Object *obj, ResetType type)
{
    PSPX86NvState *s = PSP_X86_NV(obj);

    memset(s->store, 0xff, sizeof(s->store));
}

static const VMStateDescription psp_x86_nv_vmstate = {
    .name = "psp-x86-nv",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(store, PSPX86NvState, NV_BYTES),
        VMSTATE_END_OF_LIST()
    }
};

static const Property psp_x86_nv_properties[] = {
    DEFINE_PROP_BOOL("enabled", PSPX86NvState, enabled, true),
    DEFINE_PROP_UINT64("buffer", PSPX86NvState, buffer, 0x02000000),
};

static void psp_x86_nv_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = psp_x86_nv_realize;
    dc->vmsd = &psp_x86_nv_vmstate;
    rc->phases.hold = psp_x86_nv_reset_hold;
    device_class_set_props(dc, psp_x86_nv_properties);
}

static const TypeInfo psp_x86_nv_info = {
    .name = TYPE_PSP_X86_NV,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PSPX86NvState),
    .class_init = psp_x86_nv_class_init,
};

static void psp_x86_nv_register_types(void)
{
    type_register_static(&psp_x86_nv_info);
}

type_init(psp_x86_nv_register_types)
