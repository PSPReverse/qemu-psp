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

#include "psp/x86/psp-x86.h"

#include "exec/address-spaces.h"
#include "hw/boards.h"
#include "hw/char/serial-mm.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "system/system.h"

/** BEGIN exposing the x86 AddressSpace for ASPFuzz */

PSPX86State* global_x86_state = NULL;
int32_t aspfuzz_x86_write(hwaddr addr, void* buf, hwaddr size);
int32_t aspfuzz_x86_write(hwaddr addr, void* buf, hwaddr size)
{
    if (global_x86_state == NULL) {
        return -1;
    }
    return address_space_write_rom(&(global_x86_state->as), addr,
                                   MEMTXATTRS_UNSPECIFIED, buf, size);
}
int32_t aspfuzz_x86_read(hwaddr addr, void* buf, hwaddr size);
int32_t aspfuzz_x86_read(hwaddr addr, void* buf, hwaddr size)
{
    if (global_x86_state == NULL) {
        return -1;
    }
    return address_space_read(&(global_x86_state->as), addr,
                              MEMTXATTRS_UNSPECIFIED, buf, size);
}

int32_t aspfuzz_write_catcher_activate(hwaddr addr, hwaddr size);
int32_t aspfuzz_write_catcher_activate(hwaddr addr, hwaddr size)
{
    if (global_x86_state == NULL) {
        return -1;
    }
    global_x86_state->write_catcher.base = addr;
    global_x86_state->write_catcher.size = size;
    memory_region_transaction_begin();
    memory_region_set_address(&global_x86_state->write_catcher.iomem, addr);
    memory_region_set_size(&global_x86_state->write_catcher.iomem, size);
    // Trying to get iotlb of the cpu in a valid state
    memory_region_transaction_commit();
    return 0;
}

int32_t aspfuzz_write_catcher_reset(void);
int32_t aspfuzz_write_catcher_reset(void)
{
    if (global_x86_state == NULL) {
        return -1;
    }
    memory_region_set_address(&global_x86_state->write_catcher.iomem, 0);
    memory_region_set_size(&global_x86_state->write_catcher.iomem, 0);
    global_x86_state->write_catcher.base = 0;
    global_x86_state->write_catcher.size = 0;
    global_x86_state->write_catcher.observed_illegal_read = 0;
    global_x86_state->write_catcher.pc_illegal_read = 0;
    global_x86_state->write_catcher.observed_illegal_write = 0;
    global_x86_state->write_catcher.pc_illegal_write = 0;
    return 0;
}

int32_t aspfuzz_write_catcher_status(bool get_read, hwaddr* addr, vaddr* pc);
int32_t aspfuzz_write_catcher_status(bool get_read, hwaddr* addr, vaddr* pc)
{
    if (global_x86_state == NULL) {
        return -1;
    }
    if (get_read) {
        *addr = global_x86_state->write_catcher.observed_illegal_read;
        *pc = global_x86_state->write_catcher.pc_illegal_read;
    } else {
        *addr = global_x86_state->write_catcher.observed_illegal_write;
        *pc = global_x86_state->write_catcher.pc_illegal_write;
    }
    return 0;
}

/** END exposing the x86 AddressSpace for ASPFuzz */

static void psp_x86_init(Object* obj)
{
    PSPX86State* s = PSP_X86(obj);
    object_initialize_child(obj, "misc", &s->psp_x86_misc, TYPE_PSP_X86_MISC);
    object_initialize_child(obj, "config_space", &s->pci_config_space,
                            TYPE_PSP_X86_CONFIG_SPACE);
    object_initialize_child(obj, "psp-x86-sts", &s->sts, TYPE_PSP_STS);
    object_initialize_child(obj, "write-catcher", &s->write_catcher,
                            TYPE_PSP_X86_WRITE_CATCHER);
}
/* 3.25GiB*/
#define REGULAR_DRAM_SIZE (0xD0000000)
#define REGULAR_DRAM_BASE 0x0

#define PROTECTED_REGION_BASE 0xfffd00000000UL

#define SECURE_DRAM_BASE (PROTECTED_REGION_BASE + 0xfb000000)
/* 12.5MiB*/
#define SECURE_DRAM_SIZE (125 * 1024 * 1024 / 10)

#define MMIO_BASE (PROTECTED_REGION_BASE + 0xfc000000)
#define MASTER_STATUS_PORT_BASE (MMIO_BASE + 0x80)

static void psp_x86_realize(DeviceState* dev, Error** errp)
{
    ERRP_GUARD();
    PSPX86State* s = PSP_X86(dev);
    global_x86_state = s;

    if (s->generation == ZENTESLA + 1) {
        error_setg(errp, "property 'generation' not specified for PSP X86");
        return;
    }

    /* The X86 address space. Independent from the PSP address space */
    memory_region_init(&s->mr, OBJECT(dev), "x86-memory",
                       (1UL << 48) /* 48 physical address bits */);
    address_space_init(&s->as, &s->mr, "x86 address space");

    /* Init Secure DRAM */
    memory_region_init_ram(&s->secure_dram, OBJECT(dev), "secure_dram",
                           SECURE_DRAM_SIZE, &error_abort);
    memory_region_add_subregion(&s->mr, SECURE_DRAM_BASE, &s->secure_dram);

    /* Init regular DRAM */
    memory_region_init_ram(&s->regular_dram, OBJECT(dev), "regular_dram",
                           REGULAR_DRAM_SIZE, &error_abort);
    memory_region_add_subregion(&s->mr, REGULAR_DRAM_BASE, &s->regular_dram);

    /* Map PSP Status port */
    qdev_prop_set_bit(DEVICE(&s->sts), "in_x86_space", true);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sts), errp)) {
        return;
    }
    MemoryRegion* mr_status =
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->sts), 0);
    memory_region_add_subregion(&s->mr, MASTER_STATUS_PORT_BASE, mr_status);

    /* A MMIO accessible UART */
    // serial_mm_init(&s->mr, 0xfffdfc0003f8, 0, 0, 115200, serial_hd(0),
    // DEVICE_NATIVE_ENDIAN);

    /* Connect the misc device to the x86 address space */
    qdev_prop_set_uint32(DEVICE(&s->pci_config_space), "psp_generation",
                         s->generation);
    sysbus_realize(SYS_BUS_DEVICE(&s->pci_config_space), errp);
    if (*errp) {
        return;
    }
    MemoryRegion* mr_pci_config_space =
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->pci_config_space), 0);
    memory_region_add_subregion(&s->mr, PCI_CONFIG_SPACE_BASE,
                                mr_pci_config_space);

    /* Connect the misc device to the x86 address space */
    qdev_prop_set_uint32(DEVICE(&s->psp_x86_misc), "psp_generation",
                         s->generation);
    sysbus_realize(SYS_BUS_DEVICE(&s->psp_x86_misc), errp);
    if (*errp) {
        return;
    }

    MemoryRegion* mr_x86_misc =
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->psp_x86_misc), 0);
    memory_region_add_subregion_overlap(&s->mr, 0x0, mr_x86_misc, -1000);

    /* Configure write catcher overlay device to enable catching access
    while fuzzing*/
    sysbus_realize(SYS_BUS_DEVICE(&s->write_catcher), errp);
    if (*errp) {
        return;
    }
    MemoryRegion* mr_write_catcher =
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->write_catcher), 0);
    // Positive priority, so this gets written to before all other regions
    memory_region_add_subregion_overlap(&s->mr, 0, mr_write_catcher, 1);
}

static Property properties[] = {
    DEFINE_PROP_UINT32("psp_generation", PSPX86State, generation, ZENTESLA + 1),
};

static void psp_x86_class_init(ObjectClass* oc, void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    dc->realize = psp_x86_realize;
    device_class_set_props(dc, properties);
}

static const TypeInfo psp_x86_info = {
    .name = TYPE_PSP_X86,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_init = psp_x86_init,
    .instance_size = sizeof(PSPX86State),
    .class_init = psp_x86_class_init,

};

static void psp_x86_register_types(void)
{
    type_register_static(&psp_x86_info);
}
type_init(psp_x86_register_types);
