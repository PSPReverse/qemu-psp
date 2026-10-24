#include "qemu/osdep.h"

#include "psp/smn/psp-smn-misc.h"
#include "psp/psp-regs.h"

#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
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

/* Index of the override covering the word of offset, or -1 */
static int psp_smn_misc_override(PSPSmnMiscState* s, hwaddr offset)
{
    for (uint32_t i = 0; i < s->n_overrides; i++) {
        if (s->override_offset[i] == (offset & ~(hwaddr)3)) {
            return i;
        }
    }
    return -1;
}

static uint64_t psp_smn_misc_read(void* opaque, hwaddr offset,
                                  unsigned int size)
{
    PSPSmnMiscState* s = opaque;
    PspRegHit hit;
    uint64_t val;
    int ov = psp_smn_misc_override(s, offset);

    if (ov >= 0) {
        val = (s->override_value[ov] >> ((offset & 3) * 8)) &
              MAKE_64BIT_MASK(0, size * 8);
        trace_psp_smn_misc_read_override(offset, val);
        return val;
    }
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

/*
 * Parse the overrides property: comma-separated offset=value pairs, numbers
 * C-style as accepted by qemu_strtou64 with base 0 (0x hex, leading 0 octal,
 * else decimal), offsets 4-byte aligned and unique, values 32-bit.
 */
static bool psp_smn_misc_parse_overrides(PSPSmnMiscState* s, Error** errp)
{
    g_auto(GStrv) pairs = NULL;

    s->n_overrides = 0;
    if (s->overrides == NULL || *s->overrides == '\0') {
        return true;
    }
    pairs = g_strsplit(s->overrides, ",", -1);
    for (char** p = pairs; *p != NULL; p++) {
        const char* end;
        uint64_t offset, value;

        if (s->n_overrides == PSP_SMN_MISC_MAX_OVERRIDES) {
            error_setg(errp, "overrides: more than %d pairs",
                       PSP_SMN_MISC_MAX_OVERRIDES);
            return false;
        }
        if (qemu_strtou64(*p, &end, 0, &offset) < 0 || *end != '=' ||
            qemu_strtou64(end + 1, NULL, 0, &value) < 0) {
            error_setg(errp, "overrides: '%s' is not offset=value", *p);
            return false;
        }
        if (offset > UINT32_MAX || (offset & 3) || value > UINT32_MAX) {
            error_setg(errp,
                       "overrides: '%s' needs a 4-byte aligned 32-bit offset "
                       "and a 32-bit value",
                       *p);
            return false;
        }
        if (psp_smn_misc_override(s, offset) >= 0) {
            error_setg(errp, "overrides: offset 0x%" PRIx64 " given twice",
                       offset);
            return false;
        }
        s->override_offset[s->n_overrides] = offset;
        s->override_value[s->n_overrides] = value;
        s->n_overrides++;
    }
    return true;
}

static void psp_smn_misc_realize(DeviceState* dev, Error** errp)
{
    PSPSmnMiscState* s = PSP_SMN_MISC(dev);
    if (s->generation >= PSP_GENERATION_MAX) {
        error_setg(errp, "property 'psp_smn_misc_generation' not specified or invalid");
        return;
    }
    if (!psp_smn_misc_parse_overrides(s, errp)) {
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
    DEFINE_PROP_STRING("overrides", PSPSmnMiscState, overrides),
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
