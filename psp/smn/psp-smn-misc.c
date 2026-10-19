#include "qemu/osdep.h"

#include "psp/smn/psp-smn-misc.h"

#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "trace.h"

#define PSP_SMN_MISC_LOG "[PSP SMN MISC]"

static uint64_t psp_smn_misc_read(void* opaque, hwaddr offset,
                                  unsigned int size)
{
    PSPSmnMiscState* s = opaque;
    uint64_t val;
    switch (offset) {
    case 0x51050:
        val = 0xb1aab1aa;
        break;

    case 0x5a078:
        val = 0x10;
        break;

    case 0x5a088:
        val = 0x1;
        break;

    // Zen2 ABL0
    case 0x5a08c:
        val = 0x8;
        break;

    // Zen Tesla
    case 0x5a098:
        val = 0x1;
        break;

    case 0x5a304:
        val = 0x1;
        break;

    case 0x5a870:
        val = 0x1;
        break;

    case 0x5b304:
        val = 0xffffffff;
        break;

    // Zen Tesla
    case 0x5b310:
        val = 0xffffffff;
        break;

    case 0x5bb04:
        val = 0xffffffff;
        break;

    // Zen Tesla
    case 0x5bb10:
        val = 0xffffffff;
        break;

    // Zen 2
    case 0x5c14c:
        val = 0x100;
        break;

    // Zen 3 (EPYC on-chip BL polls bit 8 of these at 0xffff1df8)
    case 0x6c14c:
    case 0x6c94c:
    case 0x6d14c:
        val = 0x100;
        break;

    // Zen 3 off-chip BL fuse read (fn 0xc7c0 via 0xc5dc, bit offset 0x5100):
    // words (1,0) select SoC class 2, which fn 0x134c8 needs (>=2) before it
    // walks the FET. Value inferred from the code, not from hardware.
    case 0x5da20:
        val = 0x1;
        break;

    // Zen Tesla
    case 0x5c310:
        val = 0xffffffff;
        break;

    // Zen 2
    case 0x5c94c:
        val = 0x100;
        break;

    case 0x5d0cc:
        val = 0x20;
        break;

        // Zen 2
        // case 0x5d154:
        //    val = 0x300;
        //    break;

    case 0x5e000:
        val = 0x1;
        break;

    // Zen Tesla
    case 0x5fb10:
        val = 0x1e113;
        break;

    // polled by Zen2 off-chip @af2a
    case 0x3b10024:
        val = 0x1;
        break;

    // polled by Zen off-chip @ae22
    case 0x3b10034:
        val = 0x1;
        break;

    // polled by Zen off-chip @827a
    case 0x3b10704:
        val = 0x1;
        break;

    // Zen 2
    case 0x9025034:
        val = 0x1e113;
        break;

    case 0x1003034:
        val = 0x1e112;
        break;

    case 0x1004034:
        val = 0x1e112;
        break;

    // Zen Tesla
    case 0x1010034:
        val = 0x1e112;
        break;

    // Zen Tesla
    case 0x1002034:
        val = 0x1e113;
        break;

    case 0x1018034:
        val = 0x1e113;
        break;

    case 0x1025034:
        val = 0x1e113;
        break;

    case 0x102e034:
        val = 0x1e312;
        break;

    case 0x1030034:
        val = 0x1e312;
        break;

    case 0x1046034:
        val = 0x1e103;
        break;

    case 0x1047034:
        val = 0x1e103;
        break;

    case 0x106c034:
        val = 0x1e113;
        break;

    case 0x106d034:
        val = 0x1e113;
        break;

    case 0x106e034:
        val = 0x1e312;
        break;

    case 0x1080034:
        val = 0x1e113;
        break;

    case 0x1081034:
        val = 0x1e113;
        break;

    case 0x1096034:
        val = 0x1e312;
        break;

    case 0x1097034:
        val = 0x1e312;
        break;

    case 0x10a8034:
        val = 0x1e312;
        break;

    case 0x10d8034:
        val = 0x1e312;
        break;

    case 0x18080064:
        val = 0x600;
        break;

    case 0x18480064:
        val = 0x600;
        break;

    case 0x2d01380:
        /* Placeholder, see psp_smn_misc_write(). Not measured. */
        val = s->placeholder_2d01380;
        break;

    default:
        /* Zen3 off-chip BL reads this bank. Not modelled: placeholder 0. */
        if (s->generation == ZEN3 && offset >= 0x5d138 && offset <= 0x5d154) {
            val = 0;
            break;
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
    PSPSmnMiscState *s = opaque;

    /* Placeholder: the value is stored unmodelled and read back. */
    if (offset == 0x2d01380) {
        s->placeholder_2d01380 = value;
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
    if (s->generation == PSP_GENERATION_MAX) {
        error_setg(errp, "property 'psp_smn_misc_generation' not specified");
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
