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

#include "system/address-spaces.h"
#include "hw/boards.h"
#include "hw/char/serial-mm.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "system/system.h"
#include "trace.h"

#include "psp/x86/psp-x86-mapper.h"

/** BEGIN exposing the x86 AddressSpace for ASPFuzz */

static PSPX86State* global_x86_state = NULL;
int32_t aspfuzz_x86_write(hwaddr addr, void* buf, hwaddr size);
int32_t aspfuzz_x86_write(hwaddr addr, void* buf, hwaddr size)
{
    if (global_x86_state == NULL || buf == NULL || size == 0 ||
        addr + size < addr) {
        return -1;
    }
    int32_t rc = address_space_write(&(global_x86_state->as), addr,
                                   MEMTXATTRS_UNSPECIFIED, buf, size);
	trace_psp_x86_misc_write(addr, rc, "write result", 0);
	return rc;
}
int32_t aspfuzz_x86_read(hwaddr addr, void* buf, hwaddr size);
int32_t aspfuzz_x86_read(hwaddr addr, void* buf, hwaddr size)
{
    if (global_x86_state == NULL || buf == NULL || size == 0 ||
        addr + size < addr) {
        return -1;
    }
    return address_space_read(&(global_x86_state->as), addr,
                              MEMTXATTRS_UNSPECIFIED, buf, size);
}
/** END exposing the x86 AddressSpace for ASPFuzz */

static void psp_x86_init(Object* obj)
{
    PSPX86State* s = PSP_X86(obj);
    object_initialize_child(obj, "misc", &s->psp_x86_misc, TYPE_PSP_X86_MISC);
    object_initialize_child(obj, "config_space", &s->pci_config_space,
                            TYPE_PSP_X86_CONFIG_SPACE);
    object_initialize_child(obj, "psp-x86-sts", &s->sts, TYPE_PSP_STS);
}
/* 3.25GiB*/
#define REGULAR_DRAM_SIZE (0x40000000)
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

    if (s->generation == PSP_GENERATION_MAX) {
        error_setg(errp, "property 'generation' not specified for PSP X86");
        return;
    }

    /* The X86 address space. Independent from the PSP address space */
    memory_region_init(&s->mr, OBJECT(dev), "x86-memory",
                       X86_PHYS_ADDR_SPACE_SIZE);
    address_space_init(&s->as, &s->mr, "x86 address space");

    /* Init Secure DRAM */
    memory_region_init_ram(&s->secure_dram, OBJECT(dev), "x86.secure_dram",
                           SECURE_DRAM_SIZE, &error_abort);
    memory_region_add_subregion(&s->mr, SECURE_DRAM_BASE, &s->secure_dram);

    /* Init regular DRAM */
    if (s->regular_dram_size == 0 || s->regular_dram_size > REGULAR_DRAM_SIZE ||
        !QEMU_IS_ALIGNED(s->regular_dram_size, qemu_real_host_page_size())) {
        error_setg(errp,
                   "property 'regular_dram_size' must be a page-aligned size "
                   "in (0, 0x%x]", REGULAR_DRAM_SIZE);
        return;
    }
    memory_region_init_ram(&s->regular_dram, OBJECT(dev), "x86.regular_dram",
                           s->regular_dram_size, &error_abort);
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
}

static const Property properties[] = {
    DEFINE_PROP_UINT32("psp_generation", PSPX86State, generation,
                       PSP_GENERATION_MAX),
    DEFINE_PROP_UINT64("regular_dram_size", PSPX86State, regular_dram_size,
                       REGULAR_DRAM_SIZE),
};

static void psp_x86_class_init(ObjectClass* oc, const void* data)
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
