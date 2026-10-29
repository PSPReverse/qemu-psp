/*
 * AMD PSP Zen3 TPM TIS stand-in.
 *
 * The Zen3 off-chip BL's TPM bring-up command touches only registers of
 * localities 0 and 3 of the TIS interface at x86 0xfed40000 and has no use
 * for a command engine. Locality 0 reads 0. Locality 3 has an ACCESS byte
 * (starts as 0x80, valid) that requestUse (write 0x02) turns into 0xa1 and
 * activeLocality release (write 0x20) into 0x81, and an STS dword that
 * reads 0x04000000 (bits 27:26 = 1, TPM 2.0). Everything else reads 0 and
 * ignores writes. With the device disabled every read is 0.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"

#include "psp/x86/x86-tpm.h"

#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "migration/vmstate.h"

#define TPM_SIZE 0x5000
#define LOC3_ACCESS 0x3000
#define LOC3_STS 0x3018
#define LOC3_STS_VALUE 0x04000000

#define ACCESS_VALID 0x80
#define ACCESS_ACTIVE 0x20
#define ACCESS_REQUEST_USE 0x02
#define ACCESS_ESTABLISHMENT 0x01

static uint8_t psp_x86_tpm_byte(PSPX86TpmState* s, hwaddr offset)
{
    if (offset == LOC3_ACCESS) {
        return s->access3;
    }
    if (offset >= LOC3_STS && offset < LOC3_STS + 4) {
        return LOC3_STS_VALUE >> (8 * (offset - LOC3_STS));
    }
    return 0;
}

static uint64_t psp_x86_tpm_read(void* opaque, hwaddr offset, unsigned size)
{
    PSPX86TpmState* s = opaque;
    uint64_t value = 0;

    if (!s->enabled) {
        return 0;
    }
    for (unsigned i = 0; i < size; i++) {
        value |= (uint64_t)psp_x86_tpm_byte(s, offset + i) << (8 * i);
    }
    return value;
}

static void psp_x86_tpm_write(void* opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    PSPX86TpmState* s = opaque;

    /* Only the ACCESS byte of locality 3 reacts; a wider write covers it */
    if (!s->enabled || LOC3_ACCESS < offset || LOC3_ACCESS >= offset + size) {
        return;
    }
    value = (value >> (8 * (LOC3_ACCESS - offset))) & 0xff;
    if (value == ACCESS_REQUEST_USE) {
        s->access3 |= ACCESS_ACTIVE | ACCESS_ESTABLISHMENT;
    } else if (value == ACCESS_ACTIVE) {
        s->access3 &= ~ACCESS_ACTIVE;
    }
}

static const MemoryRegionOps psp_x86_tpm_ops = {
    .read = psp_x86_tpm_read,
    .write = psp_x86_tpm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void psp_x86_tpm_realize(DeviceState* dev, Error** errp)
{
    PSPX86TpmState* s = PSP_X86_TPM(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &psp_x86_tpm_ops, s,
                          "psp-x86-tpm", TPM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void psp_x86_tpm_reset_hold(Object* obj, ResetType type)
{
    PSPX86TpmState* s = PSP_X86_TPM(obj);

    s->access3 = ACCESS_VALID;
}

static const VMStateDescription psp_x86_tpm_vmstate = {
    .name = "psp-x86-tpm",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(access3, PSPX86TpmState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property psp_x86_tpm_properties[] = {
    DEFINE_PROP_BOOL("enabled", PSPX86TpmState, enabled, true),
};

static void psp_x86_tpm_class_init(ObjectClass* oc, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    ResettableClass* rc = RESETTABLE_CLASS(oc);

    dc->realize = psp_x86_tpm_realize;
    dc->vmsd = &psp_x86_tpm_vmstate;
    rc->phases.hold = psp_x86_tpm_reset_hold;
    device_class_set_props(dc, psp_x86_tpm_properties);
}

static const TypeInfo psp_x86_tpm_info = {
    .name = TYPE_PSP_X86_TPM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PSPX86TpmState),
    .class_init = psp_x86_tpm_class_init,
};

static void psp_x86_tpm_register_types(void)
{
    type_register_static(&psp_x86_tpm_info);
}

type_init(psp_x86_tpm_register_types)
