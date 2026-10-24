#include "qemu/osdep.h"

#include "psp/smn/psp-smn-misc.h"
#include "psp/psp-regs.h"

#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "trace.h"

/*
 * Find the register covering the access at offset. Constants match their
 * exact offset; "rw" entries (read/write-back storage) also match a narrow
 * access inside their word.
 */
static bool psp_smn_misc_find(PSPSmnMiscState* s, hwaddr offset,
                              PspRegHit* hit)
{
    if (psp_regs_find(s->generation, PSP_REG_SPACE_SMN_MISC, offset, hit)) {
        return true;
    }
    return (offset & 3) &&
           psp_regs_find(s->generation, PSP_REG_SPACE_SMN_MISC,
                         offset & ~(hwaddr)3, hit) &&
           hit->rw_slot >= 0;
}

static uint64_t psp_smn_misc_read(void* opaque, hwaddr offset,
                                  unsigned int size)
{
    PSPSmnMiscState* s = opaque;
    PspRegHit hit;
    uint64_t val;

    if (!psp_smn_misc_find(s, offset, &hit)) {
        trace_psp_smn_misc_read_unimplemented(offset);
        return 0;
    }
    if (hit.rw_slot >= 0) {
        val = (s->rw[hit.rw_slot] >> ((offset & 3) * 8)) &
              MAKE_64BIT_MASK(0, size * 8);
    } else {
        val = hit.value;
    }
    trace_psp_smn_misc_read(offset, val);
    return val;
}

static void psp_smn_misc_write(void* opaque, hwaddr offset, uint64_t value,
                               unsigned int size)
{
    PSPSmnMiscState* s = opaque;
    PspRegHit hit;

    if (psp_smn_misc_find(s, offset, &hit) && hit.rw_slot >= 0) {
        uint32_t* slot = &s->rw[hit.rw_slot];
        uint32_t shift = (offset & 3) * 8;
        uint32_t mask = (uint32_t)MAKE_64BIT_MASK(0, size * 8) << shift;

        *slot = (*slot & ~mask) | (((uint32_t)value << shift) & mask);
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

    psp_regs_rw_reset(s->generation, PSP_REG_SPACE_SMN_MISC, s->rw);
}

static const VMStateDescription psp_smn_misc_vmstate = {
    .name = "psp-smn-misc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(rw, PSPSmnMiscState, PSP_REGS_MAX_RW_SLOTS),
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
