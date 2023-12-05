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
#include "qapi/error.h"
#include "qemu/module.h"
#include "system/system.h"
#include "exec/address-spaces.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "qemu/log.h"
#include "trace.h"

#include "psp/x86/psp-osmem.h"

static char ident[] = "PSP OSMem";
/*
static uint64_t psp_uart_read(PSPOSMemAddr offset, unsigned int size) {
    if (size != sizeof(uint8_t)) {
        qemu_log_mask(LOG_UNIMP,
                      "%s: UART Error. Unsupported read size at offset 0x%" \
                      HWADDR_PRIx "\n", ident, offset);

    }
}
*/

static uint64_t psp_osmem_read(void *opaque, hwaddr offset, unsigned int size) {
    PSPOSMemState *s = PSP_OSMEM(opaque);
    uint64_t val;
    hwaddr phys_base = s->psp_osmem_iomem.addr;

    if (size != sizeof(uint32_t)) {
        qemu_log_mask(LOG_UNIMP,
                      "%s: Error. Unsupported read size at offset 0x%" \
                      HWADDR_PRIx "\n", ident, phys_base + offset);
        return 0xdeeeeead;
    }

    unsigned slot = offset / PSP_OSMEM_CTL_SLOT_SIZE;
    unsigned reg = offset % PSP_OSMEM_CTL_SLOT_SIZE;

    if (slot >= PSP_OSMEM_NUM_SLOTS || reg != 0)
        val = 0;
    else
        val = s->psp_osmem_control[slot];

    trace_psp_osmem_control_read(offset, val);

    return val;
}

static void psp_osmem_write(void *opaque, hwaddr offset, uint64_t val,
                            unsigned int size) {
    PSPOSMemState *s = PSP_OSMEM(opaque);
    hwaddr phys = s->psp_osmem_iomem.addr;
    phys += offset;

    if (size != sizeof(uint32_t)) {
        qemu_log_mask(LOG_UNIMP,
                      "PSP OSMem: Error. Unsupported write size (%d) at offset 0x%" \
                      HWADDR_PRIx " with val 0x%lx\n", size, phys, val);
        return;
    }

    unsigned slot = offset / PSP_OSMEM_CTL_SLOT_SIZE;
    unsigned reg = offset % PSP_OSMEM_CTL_SLOT_SIZE;

    if (slot >= PSP_OSMEM_NUM_SLOTS || reg != 0) {
        qemu_log_mask(LOG_UNIMP, "PSP OSMem. Error: Unsupported MMIO access. " \
                      "offset=0x%" HWADDR_PRIx " value=0x%lx\n", offset,
                      val);
        return;
    }

    s->psp_osmem_control[slot] = val;
    trace_psp_osmem_control_write(offset, val);
}

static uint64_t psp_osmem_container_read(void *opaque, hwaddr offset, unsigned int size) {
    PSPOSMemState *s = PSP_OSMEM(opaque);
    uint64_t val;
    hwaddr phys_base = s->psp_osmem_iomem.addr;

    if (size != sizeof(uint8_t)) {
        qemu_log_mask(LOG_UNIMP,
                      "%s: Error. Unsupported read size at offset 0x%" \
                      HWADDR_PRIx "\n", ident, phys_base + offset);
        return 0;
    }

    unsigned slot = offset / PSP_OSMEM_CONTAINER_SLOT_SIZE;
    unsigned addr = offset % PSP_OSMEM_CONTAINER_SLOT_SIZE;

    val = 0;

    if (slot >= PSP_OSMEM_NUM_SLOTS) {
        qemu_log_mask(LOG_UNIMP,
                      "%s: Error. Unsupported read at offset 0x%" \
                      HWADDR_PRIx "\n", ident, phys_base + offset);
        val = 0x42;
    }

    val = s->psp_osmem_container_ram[addr];

    switch(slot) {
    case 8:
        switch(addr) {
        // uart
        case 0x3f8:
            val = 0x1;
            break;
        case 0x3f9:
            val = 0x1;
            break;
        case 0x3fb:
            val = s->uart.value;
            break;
        case 0x3fd:
            val = 0x60;
            break;
        // magic
        case 0x2d81e77:
            val = 0x7;
            break;
        }
        break;
    }

    trace_psp_osmem_container_read(slot, addr, val);

    return val;
}

static void psp_osmem_container_write(void *opaque, hwaddr offset, uint64_t val,
                                      unsigned int size) {
    PSPOSMemState *s = PSP_OSMEM(opaque);
    hwaddr phys = s->psp_osmem_iomem.addr;
    phys += offset;

    if (size != sizeof(uint8_t)) {
        qemu_log_mask(LOG_UNIMP,
                      "PSP OSMem: Error. Unsupported write size (%d) at offset 0x%" \
                      HWADDR_PRIx " with val 0x%lx\n", size, phys, val);
        return;
    }

    unsigned slot = offset / PSP_OSMEM_CONTAINER_SLOT_SIZE;
    unsigned addr = offset % 0x04000000;

    if (slot >= PSP_OSMEM_NUM_SLOTS) {
        qemu_log_mask(LOG_UNIMP, "PSP OSMem. Error: Unsupported MMIO access. " \
                      "offset=0x%" HWADDR_PRIx " value=0x%lx\n", offset,
                      val);
        return;
    }

    switch (slot) {
    case 8:
        switch (addr) {
        // uart for ZEN1
        case 0x3fb:
            s->uart.value = val;
            break;
        }
        break;
    }

    s->psp_osmem_container_ram[addr] = val;

    trace_psp_osmem_container_write(slot, addr, val);
}

static const MemoryRegionOps osmem_mem_ops = {
    .read = psp_osmem_read,
    .write = psp_osmem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static const MemoryRegionOps osmem_container_mem_ops = {
    .read = psp_osmem_container_read,
    .write = psp_osmem_container_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static void psp_osmem_init(Object *obj) {
    PSPOSMemState *s = PSP_OSMEM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    for (int i = 0; i < PSP_OSMEM_NUM_SLOTS; i++)
        s->psp_osmem_control[i] = 0;

    // uart
    s->uart.value = 0x3;

    memory_region_init_io(&s->psp_osmem_iomem, obj, &osmem_mem_ops, s,
                          TYPE_PSP_OSMEM, PSP_OSMEM_CTL_SIZE);
    sysbus_init_mmio(sbd, &s->psp_osmem_iomem);

    memory_region_init_io(&s->psp_osmem_containers_iomem, obj, &osmem_container_mem_ops, s,
                          TYPE_PSP_OSMEM, PSP_OSMEM_CONTAINER_SIZE);
    sysbus_init_mmio(sbd, &s->psp_osmem_containers_iomem);
}

static const TypeInfo psp_osmem_info = {
    .name = TYPE_PSP_OSMEM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_init = psp_osmem_init,
    .instance_size = sizeof(PSPOSMemState),
};

static void psp_osmem_register_types(void) {
    type_register_static(&psp_osmem_info);
}

type_init(psp_osmem_register_types);
