/*
 * AMD PSP emulation
 *
 * Copyright (C) 2020 Robert Buhren <robert@robertbuhren.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "qemu/osdep.h"

#include "psp/smn/psp-smn.h"

#include "system/address-spaces.h"
#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "trace.h"

#include "psp/psp-regs.h"
#include "psp/psp.h"
#include "psp/util.h"

/* PSPSmnCTRL: Controller, attaches SMN devices on demand? */

//// +++ Begin ASPFuzz code +++
uint32_t aspfuzz_smn_slots[32] = {0};
PSPSmnState* aspfuzz_smn_state = 0;

void aspfuzz_smn_update_slot(uint32_t idx);
void aspfuzz_smn_update_slot(uint32_t idx)
{
    PSPSmnAddr addr = aspfuzz_smn_slots[idx];
    psp_window_set_target(&aspfuzz_smn_state->psp_smn_windows[idx], addr);
    aspfuzz_smn_state->psp_smn_alias[idx] = addr;
    trace_psp_smn_update_slot(idx, addr);
}

AddressSpace* global_smn_as = NULL;
int aspfuzz_smn_write(hwaddr addr, void* buf, int size);
int aspfuzz_smn_write(hwaddr addr, void* buf, int size)
{
    if (global_smn_as == NULL || buf == NULL || size <= 0) {
        return -1;
    }
    return address_space_write_rom(global_smn_as, addr, MEMTXATTRS_UNSPECIFIED,
                                   buf, size);
}
int aspfuzz_smn_read(hwaddr addr, void* buf, int size);
int aspfuzz_smn_read(hwaddr addr, void* buf, int size)
{
    if (global_smn_as == NULL || buf == NULL || size <= 0) {
        return -1;
    }
    return address_space_read(global_smn_as, addr, MEMTXATTRS_UNSPECIFIED, buf,
                              size);
}

//// +++ End ASPFuzz code +++

typedef struct PspSmnClass {
    /*< private >*/
    SysBusDeviceClass parent_class;
    /*< public >*/
    hwaddr flash;
    PspGeneration gen;
} PspSmnClass;

#define PSP_SMN_CLASS(klass)                                                   \
    OBJECT_CLASS_CHECK(PspSmnClass, (klass), TYPE_PSP_SMN)
#define PSP_SMN_GET_CLASS(obj)                                                 \
    OBJECT_GET_CLASS(PspSmnClass, (obj), TYPE_PSP_SMN)

typedef struct PspSmnConfiguration {
    const char* desc;
    hwaddr flash;
} PspSmnConfiguration;

static PspSmnConfiguration psp_smn_configuration[] = {
    [ZEN] =
        {
            .desc = "psp smn zen",
            .flash = 0x0a000000,
        },
    [ZEN_PLUS] =
        {
            .desc = "psp smn zen+",
            .flash = 0x0a000000,
        },
    [ZEN2] =
        {
            .desc = "psp smn zen2",
            .flash = 0x44000000,
        },
    [ZEN3] =
        {
            .desc = "psp smn zen3",
            .flash = 0x44000000,
        },
    [ZENTESLA] =
        {
            .desc = "psp smn zentesla",
            .flash = 0x0a000000,
        },
};

const char* ident = "SMN Control";

static void psp_smn_set_alias(PSPSmnState *smn, uint32_t idx, hwaddr offset)
{
    /* TODO documentation */
    smn->psp_smn_alias[idx] = offset;
    psp_window_set_target(&smn->psp_smn_windows[idx], offset);
}

static void psp_smn_mirror_slot(PSPSmnState *smn, uint32_t idx)
{
    //// +++ Begin ASPFuzz code +++
    aspfuzz_smn_state = (void*)smn;
    aspfuzz_smn_slots[idx] = smn->psp_smn_slots[idx];
    //// +++ End ASPFuzz code +++
}

static void psp_smn_update_slot(PSPSmnState *smn, uint32_t idx)
{
    psp_smn_set_alias(smn, idx, smn->psp_smn_slots[idx]);
    trace_psp_smn_update_slot(idx, smn->psp_smn_slots[idx]);
    psp_smn_mirror_slot(smn, idx);
}

static void psp_smn_write(void* opaque, hwaddr offset, uint64_t value,
                          unsigned int size)
{
    PSPSmnState* smn = PSP_SMN(opaque);
    uint32_t idx;
    hwaddr phys_base = smn->psp_smn_control.addr;

    trace_psp_smn_write(offset, phys_base, value, size);

    switch (size) {
    case 4:
        idx = (offset / 4) * 2;
        smn->psp_smn_slots[idx] = ((uint32_t)value & 0xffff) << 20;
        smn->psp_smn_slots[idx + 1] = ((uint32_t)value >> 16) << 20;
        psp_window_begin_update();
        psp_smn_update_slot(smn, idx);
        psp_smn_update_slot(smn, idx + 1);
        psp_window_end_update();
        break;
    case 2:
        idx = offset / 2;
        smn->psp_smn_slots[idx] = (uint32_t)value << 20;
        psp_smn_update_slot(smn, idx);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: SMN ERROR. Unsupported write access size: %d", ident,
                      size);
        break;
    }
}

static uint64_t psp_smn_read(void* opaque, hwaddr offset, unsigned int size)
{

    PSPSmnState* smn = PSP_SMN(opaque);
    uint32_t idx;
    uint32_t val = 0;
    hwaddr phys_base = smn->psp_smn_control.addr;

    switch (size) {
    case 4:
        /* Each 4 byte access programs two slots.
         * SMN regions are 1M large, thus each SMN address is 1M aligned.
         */
        idx = (offset / 4) * 2;
        val = ((smn->psp_smn_slots[idx + 1] >> 20) << 16) |
              (smn->psp_smn_slots[idx] >> 20);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: SMN ERROR. Unsupported read access size: %d\n",
                      ident, size);
        break;
    }
    trace_psp_smn_read(offset, phys_base, val, size);
    return val;
}

/*
 * The slot registers and the applied alias offsets are restored as they were
 * saved; the windows are then pointed at those offsets again.
 */
static int psp_smn_post_load(void *opaque, int version_id)
{
    PSPSmnState *smn = PSP_SMN(opaque);

    psp_window_begin_update();
    for (int i = 0; i < PSP_SMN_SLOT_COUNT; i++) {
        psp_smn_set_alias(smn, i, smn->psp_smn_alias[i]);
        psp_smn_mirror_slot(smn, i);
    }
    psp_window_end_update();
    return 0;
}

static const VMStateDescription psp_smn_vmstate = {
    .name = TYPE_PSP_SMN,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = psp_smn_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(psp_smn_slots, PSPSmnState, PSP_SMN_SLOT_COUNT),
        VMSTATE_UINT64_ARRAY(psp_smn_alias, PSPSmnState, PSP_SMN_SLOT_COUNT),
        VMSTATE_END_OF_LIST()
    }
};

static const MemoryRegionOps smn_ctlr_ops = {
    .read = psp_smn_read,
    .write = psp_smn_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/**
 * @brief Initializes object on the SMN bus
 *
 * @param obj -  PSPSmnState object
 */
static void psp_smn_init(Object* obj)
{
    PSPSmnState* s = PSP_SMN(obj);
    /* SysBusDevice *sbd = SYS_BUS_DEVICE(obj); */

    /* Base address of the SMN containers in the SMN address space */

    object_initialize_child(obj, "smn_misc", &s->psp_smn_misc,
                            TYPE_PSP_SMN_MISC);
    /* Only Zen3 has the SMBus controller, the SMU mailbox, the UMC APB
     * windows and the DXIO PCS registers */
    if (PSP_SMN_GET_CLASS(obj)->gen == ZEN3) {
        object_initialize_child(obj, "smn_smbus", &s->psp_smn_smbus,
                                TYPE_PSP_SMN_SMBUS);
        object_initialize_child(obj, "smn_dxio_pcs", &s->psp_smn_dxio_pcs,
                                TYPE_PSP_SMN_DXIO_PCS);
        object_initialize_child(obj, "smn_smu", &s->psp_smn_smu,
                                TYPE_PSP_SMN_SMU);
        object_initialize_child(obj, "smn_mp5", &s->psp_smn_mp5,
                                TYPE_PSP_SMN_MP5);
        object_initialize_child(obj, "smn_umc", &s->psp_smn_umc,
                                TYPE_PSP_SMN_UMC);
    }
    object_initialize_child(obj, "smn_flash", &s->psp_smn_flash,
                            TYPE_PSP_SMN_FLASH);
}

/**
 * @brief Initializes the mapping setup
 *
 * @param dev - PSPSmnState
 */
static void psp_smn_realize_slots(PSPSmnState* s)
{
    int i;
    char name[PSP_SMN_SLOT_NAME_LEN] = {0};
    hwaddr slot_offset;

    for (i = 0; i < PSP_SMN_SLOT_COUNT; i++) {
        /* The SMN MMIO region of the PSP */
        snprintf(name, PSP_SMN_SLOT_NAME_LEN, "%s%d", PSP_SMN_SLOT_NAME, i);

        /* Each window shows the SMN address space, initially from address 0
         * of the slot's own 1M, see psp_window_init().
         */
        slot_offset = s->psp_smn_base + i * PSP_SMN_SLOT_SIZE;

        /* Map the windows to the PSP address space */
        psp_window_init(&s->psp_smn_windows[i], OBJECT(s), name,
                        get_system_memory(), slot_offset, &s->psp_smn_space,
                        &s->psp_smn_space_as, i * PSP_SMN_SLOT_SIZE,
                        PSP_SMN_SLOT_SIZE);
    }
}

static void psp_smn_realize(DeviceState* dev, Error** errp)
{
    ERRP_GUARD();

    PSPSmnState* s = PSP_SMN(dev);
    PspSmnClass* c = PSP_SMN_GET_CLASS(dev);
    SysBusDevice* sbd = SYS_BUS_DEVICE(dev);

    qdev_prop_set_uint32(DEVICE(&s->psp_smn_misc), "psp_smn_misc_generation",
                         c->gen);
    CALL_AND_CHECK(sysbus_realize, SYS_BUS_DEVICE(&s->psp_smn_misc), errp);
    CALL_AND_CHECK(sysbus_realize, SYS_BUS_DEVICE(&s->psp_smn_flash), errp);

    /* The SMN address space. Independent from the PSP address space */
    memory_region_init(&s->psp_smn_space, OBJECT(dev), "smn-memory",
                       (1UL << 32) - 1);
    address_space_init(&s->psp_smn_space_as, &s->psp_smn_space,
                       "smn-address-space");
    global_smn_as = &s->psp_smn_space_as;

    /* The SMN MMIO regions containing the control regs */
    memory_region_init_io(&s->psp_smn_control, OBJECT(dev), &smn_ctlr_ops, s,
                          TYPE_PSP_SMN, PSP_SMN_CTRL_SIZE);

    sysbus_init_mmio(sbd, &s->psp_smn_control);

    MemoryRegion* mr_smn_misc =
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->psp_smn_misc), 0);
    memory_region_add_subregion_overlap(&s->psp_smn_space, 0x0, mr_smn_misc,
                                        -1000);

    /* Map the flash region into the SMN address space */
    PSPSmnFlashState* flash = &s->psp_smn_flash;
    memory_region_add_subregion_overlap(&s->psp_smn_space, c->flash,
                                        &flash->psp_smn_flash, 0);

    /* The Zen3 off-chip BL reads the flash through SMN 0x47000000, the on-chip
     * BL through 0x44000000: expose the same image at both. */
    if (c->gen == ZEN3) {
        memory_region_init_alias(&s->psp_smn_flash_alias, OBJECT(dev),
                                 "smn-flash-alias", &flash->psp_smn_flash, 0,
                                 PSP_SMN_FLASH_SIZE_16);
        memory_region_add_subregion_overlap(&s->psp_smn_space, 0x47000000,
                                            &s->psp_smn_flash_alias, 0);
    }

    if (c->gen == ZEN3) {
        CALL_AND_CHECK(sysbus_realize, SYS_BUS_DEVICE(&s->psp_smn_smbus),
                       errp);
        memory_region_add_subregion_overlap(
            &s->psp_smn_space, PSP_SMN_SMBUS_BASE,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->psp_smn_smbus), 0), 0);
        CALL_AND_CHECK(sysbus_realize, SYS_BUS_DEVICE(&s->psp_smn_smu), errp);
        memory_region_add_subregion_overlap(
            &s->psp_smn_space, PSP_SMN_SMU_BASE,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->psp_smn_smu), 0), 0);
        CALL_AND_CHECK(sysbus_realize, SYS_BUS_DEVICE(&s->psp_smn_mp5), errp);
        memory_region_add_subregion_overlap(
            &s->psp_smn_space, PSP_SMN_MP5_BASE,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->psp_smn_mp5), 0), 0);
        CALL_AND_CHECK(sysbus_realize, SYS_BUS_DEVICE(&s->psp_smn_dxio_pcs),
                       errp);
        for (unsigned i = 0; i < PSP_SMN_DXIO_PCS_COUNT; i++) {
            memory_region_add_subregion_overlap(
                &s->psp_smn_space,
                ((hwaddr)(PSP_SMN_DXIO_PCS_FIRST + i)
                 * PSP_SMN_DXIO_PCS_STRIDE) + PSP_SMN_DXIO_PCS_OFFSET,
                sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->psp_smn_dxio_pcs),
                                       i), 0);
        }
        CALL_AND_CHECK(sysbus_realize, SYS_BUS_DEVICE(&s->psp_smn_umc), errp);
        for (unsigned i = 0; i < PSP_SMN_UMC_CHANNELS; i++) {
            memory_region_add_subregion_overlap(
                &s->psp_smn_space,
                PSP_SMN_UMC_BASE + (hwaddr)i * PSP_SMN_UMC_STRIDE,
                sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->psp_smn_umc), i), 0);
            memory_region_add_subregion_overlap(
                &s->psp_smn_space,
                PSP_SMN_UMC_CSR_BASE + (hwaddr)i * PSP_SMN_UMC_STRIDE,
                sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->psp_smn_umc),
                                       PSP_SMN_UMC_CHANNELS + i), 0);
        }
    }

    /* RAM windows from the JSON register tables in psp/regs */
    size_t nwin;
    const PspRamWindow* win = psp_regs_ram_windows(c->gen, &nwin);
    assert(nwin <= PSP_SMN_MAX_RAM_WINDOWS);
    for (size_t i = 0; i < nwin; i++) {
        g_autofree char* name = g_strdup_printf("smn-ram-%" PRIx64,
                                                win[i].offset);
        memory_region_init_ram(&s->psp_smn_ram[i], OBJECT(dev), name,
                               win[i].size, errp);
        memory_region_add_subregion_overlap(&s->psp_smn_space, win[i].offset,
                                            &s->psp_smn_ram[i], 10);
    }

    /* Setup the initial SMN to PSP MemoryRegion alias. */
    psp_smn_realize_slots(s);
}

/*
 * Back to the power-on state: no slot programmed, each container at its
 * identity offset (see psp_smn_realize_slots()).
 */
static void psp_smn_reset_hold(Object *obj, ResetType type)
{
    PSPSmnState *smn = PSP_SMN(obj);

    psp_window_begin_update();
    for (int i = 0; i < PSP_SMN_SLOT_COUNT; i++) {
        smn->psp_smn_slots[i] = 0;
        psp_smn_set_alias(smn, i, i * PSP_SMN_SLOT_SIZE);
        psp_smn_mirror_slot(smn, i);
    }
    psp_window_end_update();
}

static void psp_smn_class_init(ObjectClass* oc, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = psp_smn_realize;
    dc->vmsd = &psp_smn_vmstate;
    rc->phases.hold = psp_smn_reset_hold;
}

static const Property psp_smn_properties[] = {
    DEFINE_PROP_UINT64("smn-container-base", PSPSmnState, psp_smn_base, 0),
};

static void psp_smn_common_class_init(ObjectClass* oc, enum PspGeneration gen)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    PspSmnClass* sc = PSP_SMN_CLASS(oc);

    dc->desc = psp_smn_configuration[gen].desc;
    sc->flash = psp_smn_configuration[gen].flash;
    sc->gen = gen;
    device_class_set_props(dc, psp_smn_properties);
}

static void psp_smn_zen_class_init(ObjectClass* oc, const void* data)
{
    psp_smn_common_class_init(oc, ZEN);
}

static void psp_smn_zenplus_class_init(ObjectClass* oc, const void* data)
{
    psp_smn_common_class_init(oc, ZEN_PLUS);
}

static void psp_smn_zentwo_class_init(ObjectClass* oc, const void* data)
{
    psp_smn_common_class_init(oc, ZEN2);
}

static void psp_smn_zenthree_class_init(ObjectClass* oc, const void* data)
{
    psp_smn_common_class_init(oc, ZEN3);
}

static void psp_smn_zentesla_class_init(ObjectClass* oc, const void* data)
{
    psp_smn_common_class_init(oc, ZENTESLA);
}

static const TypeInfo amd_psp_smn_types[] = {
    {
        .name = TYPE_PSP_SMN_ZEN,
        .parent = TYPE_PSP_SMN,
        .class_init = psp_smn_zen_class_init,
    },
    {
        .name = TYPE_PSP_SMN_ZEN_PLUS,
        .parent = TYPE_PSP_SMN,
        .class_init = psp_smn_zenplus_class_init,
    },
    {
        .name = TYPE_PSP_SMN_ZEN2,
        .parent = TYPE_PSP_SMN,
        .class_init = psp_smn_zentwo_class_init,
    },
    {
        .name = TYPE_PSP_SMN_ZEN3,
        .parent = TYPE_PSP_SMN,
        .class_init = psp_smn_zenthree_class_init,
    },
    {
        .name = TYPE_PSP_SMN_ZENTESLA,
        .parent = TYPE_PSP_SMN,
        .class_init = psp_smn_zentesla_class_init,
    },
    {
        .name = TYPE_PSP_SMN,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_init = psp_smn_init,
        .instance_size = sizeof(PSPSmnState),
        .class_size = sizeof(PspSmnClass),
        .class_init = psp_smn_class_init,
        .abstract = true,
    }};

DEFINE_TYPES(amd_psp_smn_types);
