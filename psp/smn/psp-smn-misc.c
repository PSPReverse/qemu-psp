#include "qemu/osdep.h"

#include "psp/smn/psp-smn-misc.h"
#include "psp/psp-regs.h"

#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "migration/vmstate.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "trace.h"

static bool psp_smn_misc_in_scratch(PSPSmnMiscState* s, hwaddr offset,
                                    unsigned int size)
{
    return s->generation == ZEN3 && offset >= PSP_SMN_SCRATCH_BASE &&
           offset + size <= PSP_SMN_SCRATCH_BASE + PSP_SMN_SCRATCH_SIZE;
}

static uint64_t psp_smn_misc_read(void* opaque, hwaddr offset,
                                  unsigned int size)
{
    PSPSmnMiscState* s = opaque;
    uint64_t val;

    if (psp_smn_misc_in_scratch(s, offset, size)) {
        val = ldn_le_p(&s->scratch[offset - PSP_SMN_SCRATCH_BASE], size);
        trace_psp_smn_misc_read(offset, val);
        return val;
    }

    /* Placeholder, see psp_smn_misc_write(). Not measured. */
    if ((offset & ~(hwaddr)3) == 0x2d01380) {
        val = (s->placeholder_2d01380 >> ((offset & 3) * 8)) &
              MAKE_64BIT_MASK(0, size * 8);
        trace_psp_smn_misc_read(offset, val);
        return val;
    }

    if (!psp_regs_lookup(s->generation, PSP_REG_SPACE_SMN_MISC, offset, &val)) {
        /* Zen3 off-chip BL reads this bank. Not modelled: placeholder 0. */
        if (s->generation == ZEN3 && offset >= 0x5d138 && offset <= 0x5d154) {
            trace_psp_smn_misc_read(offset, 0);
            return 0;
        }
        trace_psp_smn_misc_read_unimplemented(offset);
        return 0;
    }

    trace_psp_smn_misc_read(offset, val);
    return val;
}

static void psp_smn_misc_write(void* opaque, hwaddr offset, uint64_t value,
                               unsigned int size)
{
    PSPSmnMiscState* s = opaque;

    if (psp_smn_misc_in_scratch(s, offset, size)) {
        stn_le_p(&s->scratch[offset - PSP_SMN_SCRATCH_BASE], size, value);
        return;
    }

    /* Placeholder: the value is stored unmodelled and read back. */
    if ((offset & ~(hwaddr)3) == 0x2d01380) {
        uint32_t shift = (offset & 3) * 8;
        uint32_t mask = (uint32_t)MAKE_64BIT_MASK(0, size * 8) << shift;

        s->placeholder_2d01380 = (s->placeholder_2d01380 & ~mask) |
                                 (((uint32_t)value << shift) & mask);
        trace_psp_smn_misc_write(offset, value);
        return;
    }
    trace_psp_smn_misc_write_unimplemented(offset, value);
}

static const MemoryRegionOps misc_mem_ops = {
    .read = psp_smn_misc_read,
    .write = psp_smn_misc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void psp_smn_misc_realize(DeviceState* dev, Error** errp)
{
    PSPSmnMiscState* s = PSP_SMN_MISC(dev);
    if (s->generation >= PSP_GENERATION_MAX) {
        error_setg(errp, "property 'psp_smn_misc_generation' not specified or invalid");
        return;
    }
    /* Init mmio */
    memory_region_init_io(&s->iomem, OBJECT(dev), &misc_mem_ops, s,
                          "psp-smn-misc", UINT32_MAX);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void psp_smn_misc_reset_hold(Object* obj, ResetType type)
{
    PSPSmnMiscState* s = PSP_SMN_MISC(obj);

    s->placeholder_2d01380 = 0;
    memset(s->scratch, 0, sizeof(s->scratch));
}

static const VMStateDescription psp_smn_misc_vmstate = {
    .name = "psp-smn-misc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(placeholder_2d01380, PSPSmnMiscState),
        VMSTATE_BUFFER(scratch, PSPSmnMiscState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property psp_smn_misc_properties[] = {
    DEFINE_PROP_UINT32("psp_smn_misc_generation", PSPSmnMiscState, generation,
                       PSP_GENERATION_MAX),
};

static void psp_smn_misc_class_init(ObjectClass* oc, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    ResettableClass* rc = RESETTABLE_CLASS(oc);

    dc->realize = psp_smn_misc_realize;
    dc->vmsd = &psp_smn_misc_vmstate;
    rc->phases.hold = psp_smn_misc_reset_hold;
    device_class_set_props(dc, psp_smn_misc_properties);
}

static const TypeInfo psp_smn_misc_info = {
    .name = TYPE_PSP_SMN_MISC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PSPSmnMiscState),
    .class_init = psp_smn_misc_class_init,
};

static void psp_misc_register_types(void)
{
    type_register_static(&psp_smn_misc_info);
}

type_init(psp_misc_register_types);
