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

#include "exec/address-spaces.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qemu/log-for-trace.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/system.h"
#include "trace.h"

#include "psp/psp.h"

/* PSPSmnCTRL: Controller, attaches SMN devices on demand? */

//// +++ Begin ASPFuzz code +++
uint32_t aspfuzz_smn_slots[32] = {0};
PSPSmnState* aspfuzz_smn_state = 0;

void aspfuzz_smn_update_slot(uint32_t idx);
void aspfuzz_smn_update_slot(uint32_t idx)
{
    PSPSmnAddr addr = aspfuzz_smn_slots[idx];
    memory_region_set_alias_offset(&aspfuzz_smn_state->psp_smn_containers[idx],
                                   addr);
    trace_psp_smn_update_slot(idx, addr);
}
//// +++ End ASPFuzz code +++

typedef struct PspSmnClass {
    /*< private >*/
    SysBusDeviceClass parent_class;
    /*< public >*/
    hwaddr flash;
    hwaddr smu;
    PspGeneration gen;
} PspSmnClass;

#define PSP_SMN_CLASS(klass)                                                   \
    OBJECT_CLASS_CHECK(PspSmnClass, (klass), TYPE_PSP_SMN)
#define PSP_SMN_GET_CLASS(obj)                                                 \
    OBJECT_GET_CLASS(PspSmnClass, (obj), TYPE_PSP_SMN)

typedef struct PspSmnConfiguration {
    const char* desc;
    hwaddr flash;
    hwaddr smu;
} PspSmnConfiguration;

static PspSmnConfiguration psp_smn_configuration[] = {
    [ZEN] =
        {
            .desc = "psp smn zen",
            .flash = 0x0a000000,
            .smu = 0x03c00000,
        },
    [ZEN_PLUS] =
        {
            .desc = "psp smn zen+",
            .flash = 0x0a000000,
            .smu = 0x03c00000,
        },
    [ZEN2] =
        {
            .desc = "psp smn zen2",
            .flash = 0x44000000,
            .smu = 0x03c00000,
        },
    [ZEN3] =
        {
            .desc = "psp smn zen3",
            .flash = 0x44000000,
            .smu = 0x03c00000,
        },
    [ZENTESLA] =
        {
            .desc = "psp smn zentesla",
            .flash = 0x0a000000,
            .smu = 0x03c00000,
        },
};

const char* ident = "SMN Control";

static void psp_smn_update_slot(PSPSmnState* smn, uint32_t idx)
{
    PSPSmnAddr addr = smn->psp_smn_slots[idx];
    /* TODO documentation */
    memory_region_set_alias_offset(&smn->psp_smn_containers[idx], addr);
    trace_psp_smn_update_slot(idx, addr);

    //// +++ Begin ASPFuzz code +++
    aspfuzz_smn_state = (void*)smn;
    aspfuzz_smn_slots[idx] = addr;
    //// +++ End ASPFuzz code +++
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
        psp_smn_update_slot(smn, idx);
        psp_smn_update_slot(smn, idx + 1);
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
    object_initialize_child(obj, "smn_flash", &s->psp_smn_flash,
                            TYPE_PSP_SMN_FLASH);

    object_initialize_child(obj, "smn_smu", &s->psp_smn_smu, TYPE_PSP_SMN_SMU);
}

/**
 * @brief Initializes the mapping setup
 *
 * @param dev - PSPSmnState
 */
static void psp_smn_init_slots(DeviceState* dev)
{
    PSPSmnState* s = PSP_SMN(dev);
    int i;
    char name[PSP_SMN_SLOT_NAME_LEN] = {0};
    hwaddr slot_offset;

    for (i = 0; i < PSP_SMN_SLOT_COUNT; i++) {
        /* The SMN MMIO region of the PSP */
        snprintf(name, PSP_SMN_SLOT_NAME_LEN, "%s%d", PSP_SMN_SLOT_NAME, i);

        /* TODO: How to check for errors? */
        /* Each container is an alias to the SMN address space. Here we
         * initialize each container beginning with the address 0 in the SMN
         * address space.
         */
        memory_region_init_alias(&s->psp_smn_containers[i], OBJECT(dev), name,
                                 &s->psp_smn_space, i * PSP_SMN_SLOT_SIZE,
                                 PSP_SMN_SLOT_SIZE);

        /* Map the containers to the PSP address space */
        slot_offset = s->psp_smn_base + i * PSP_SMN_SLOT_SIZE;

        memory_region_add_subregion_overlap(get_system_memory(), slot_offset,
                                            &s->psp_smn_containers[i], 0);
    }
}

static void psp_smn_realize(DeviceState* dev, Error** errp)
{
    PSPSmnState* s = PSP_SMN(dev);
    PspSmnClass* c = PSP_SMN_GET_CLASS(dev);
    SysBusDevice* sbd = SYS_BUS_DEVICE(dev);
    MemoryRegion* mr_smn_misc;
    PSPSmnFlashState* flash;
    PSPSmnSmuState* smu;
    Error* err = NULL;

    qdev_prop_set_uint32(DEVICE(&s->psp_smn_misc), "psp_smn_misc_generation",
                         c->gen);
    sysbus_realize(SYS_BUS_DEVICE(&s->psp_smn_misc), &err);
    sysbus_realize(SYS_BUS_DEVICE(&s->psp_smn_flash), &err);
    sysbus_realize(SYS_BUS_DEVICE(&s->psp_smn_smu), &err);
    if (err != NULL) {
        error_propagate(errp, err);
        return;
    }

    /* The SMN address space. Independent from the PSP address space */
    memory_region_init(&s->psp_smn_space, OBJECT(dev), "smn-address-space",
                       (1UL << 32) - 1);

    /* The SMN MMIO regions containing the control regs */
    memory_region_init_io(&s->psp_smn_control, OBJECT(dev), &smn_ctlr_ops, s,
                          TYPE_PSP_SMN, PSP_SMN_CTRL_SIZE);

    sysbus_init_mmio(sbd, &s->psp_smn_control);

    mr_smn_misc = sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->psp_smn_misc), 0);
    memory_region_add_subregion_overlap(&s->psp_smn_space, 0x0, mr_smn_misc,
                                        -1000);

    /* Map the flash region into the SMN address space */
    flash = &s->psp_smn_flash;
    memory_region_add_subregion_overlap(&s->psp_smn_space, c->flash,
                                        &flash->psp_smn_flash, 0);

    smu = &s->psp_smn_smu;
    memory_region_add_subregion_overlap(&s->psp_smn_space, c->smu,
                                        &smu->psp_smn_smu, 0);

    /* Setup the initial SMN to PSP MemoryRegion alias. */
    psp_smn_init_slots(dev);
}

static void psp_smn_class_init(ObjectClass* oc, void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    dc->realize = psp_smn_realize;
}

static Property psp_smn_properties[] = {
    DEFINE_PROP_UINT64("smn-container-base", PSPSmnState, psp_smn_base, 0),
};

static void psp_smn_common_class_init(ObjectClass* oc, enum PspGeneration gen)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    PspSmnClass* sc = PSP_SMN_CLASS(oc);

    dc->desc = psp_smn_configuration[gen].desc;
    sc->flash = psp_smn_configuration[gen].flash;
    sc->smu = psp_smn_configuration[gen].smu;
    sc->gen = gen;
    device_class_set_props(dc, psp_smn_properties);
}

static void psp_smn_zen_class_init(ObjectClass* oc, void* data)
{
    psp_smn_common_class_init(oc, ZEN);
}

static void psp_smn_zenplus_class_init(ObjectClass* oc, void* data)
{
    psp_smn_common_class_init(oc, ZEN_PLUS);
}

static void psp_smn_zentwo_class_init(ObjectClass* oc, void* data)
{
    psp_smn_common_class_init(oc, ZEN2);
}

static void psp_smn_zenthree_class_init(ObjectClass* oc, void* data)
{
    psp_smn_common_class_init(oc, ZEN3);
}

static void psp_smn_zentesla_class_init(ObjectClass* oc, void* data)
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
