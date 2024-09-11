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

static void psp_x86_init(Object* obj)
{
    PSPX86State* s = PSP_X86(obj);
    object_initialize_child(obj, "misc", &s->psp_x86_misc, TYPE_PSP_X86_MISC);
}
/* 12.5MiB*/
#define SECURE_DRAM_SIZE (125 * 1024 * 1024 / 10)
/* TODO: First guess, needs to be updated*/
#define SECURE_DRAM_BASE 0xfffdfb000000

static void psp_x86_realize(DeviceState* dev, Error** errp)
{
    ERRP_GUARD();
    PSPX86State* s = PSP_X86(dev);

    if (s->generation == ZENTESLA + 1) {
        error_setg(errp, "property 'generation' not specified for PSP X86");
        return;
    }

    if (s->generation == ZENTESLA + 1) {
        error_setg(errp, "property 'generation' not specified for PSP X86");
        return;
    }
    /* The X86 address space. Independent from the PSP address space */
    memory_region_init(&s->mr, OBJECT(dev), "x86-memory",
                       (1UL << 48) /* 48 physical address bits */);
    address_space_init(&s->as, &s->mr, "x86 address space");

    /* Init SRAM */
    memory_region_init_ram(&s->secure_dram, OBJECT(dev), "secure_dram",
                           SECURE_DRAM_SIZE, &error_abort);
    memory_region_add_subregion(&s->mr, SECURE_DRAM_BASE, &s->secure_dram);
    // int constant = 0x60;
    // MemTxResult res = address_space_write(&s->as, 0xfffdfc0003fd,
    // MEMTXATTRS_UNSPECIFIED, &constant, 1); if (res != MEMTX_OK) {
    //     error_setg(errp, "Failed to write to x86 address space");
    //     return;
    // }
    /* A MMIO accessible UART */
    // serial_mm_init(&s->mr, 0xfffdfc0003f8, 0, 0, 115200, serial_hd(0),
    // DEVICE_NATIVE_ENDIAN);

    /* Connect the misc device to the x86 address space */
    // TODO: Add RAM to the x86 address space
    qdev_prop_set_uint32(DEVICE(&s->psp_x86_misc), "psp_generation",
                         s->generation);

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
