#include "qemu/osdep.h"

#include "psp/smn/psp-smn-misc.h"
#include "psp/psp-regs.h"

#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "trace.h"

static uint64_t psp_smn_misc_read(void* opaque, hwaddr offset,
                                  unsigned int size)
{
    PSPSmnMiscState* s = opaque;
    uint64_t val;

    if (!psp_regs_lookup(s->generation, PSP_REG_SPACE_SMN_MISC, offset, &val)) {
        trace_psp_smn_misc_read_unimplemented(offset);
        return 0;
    }

    trace_psp_smn_misc_read(offset, val);
    return val;
}

static void psp_smn_misc_write(void* opaque, hwaddr offset, uint64_t value,
                               unsigned int size)
{
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

static const Property psp_smn_misc_properties[] = {
    DEFINE_PROP_UINT32("psp_smn_misc_generation", PSPSmnMiscState, generation,
                       PSP_GENERATION_MAX),
};

static void psp_smn_misc_class_init(ObjectClass* oc, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    dc->realize = psp_smn_misc_realize;
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
