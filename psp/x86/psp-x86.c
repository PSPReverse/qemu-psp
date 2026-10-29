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
#include "qemu/cutils.h"
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

#define X86_POKE_MAX 4096

/*
 * Write-only "poke" property: "ADDR,HEXBYTES". ADDR is a hex x86 physical
 * address, HEXBYTES up to 4096 bytes in hex. Written with unspecified
 * attributes, as a host write would be.
 */
static void psp_x86_set_poke(Object* obj, const char* str, Error** errp)
{
    PSPX86State* s = PSP_X86(obj);
    const char* comma = strchr(str, ',');
    g_autofree char* astr = NULL;
    g_autofree uint8_t* buf = NULL;
    uint64_t addr;
    size_t len, i;

    if (!comma) {
        error_setg(errp, "expected \"ADDR,HEXBYTES\"");
        return;
    }
    astr = g_strndup(str, comma - str);
    if (qemu_strtou64(astr, NULL, 16, &addr) < 0) {
        error_setg(errp, "bad hex address");
        return;
    }
    len = strlen(comma + 1);
    if (len == 0 || len % 2 != 0) {
        error_setg(errp, "data must be a non-empty, even number of hex digits");
        return;
    }
    len /= 2;
    if (len > X86_POKE_MAX) {
        error_setg(errp, "at most %d bytes", X86_POKE_MAX);
        return;
    }
    if (addr >= X86_PHYS_ADDR_SPACE_SIZE ||
        len > X86_PHYS_ADDR_SPACE_SIZE - addr) {
        error_setg(errp, "range outside the x86 address space");
        return;
    }
    buf = g_malloc(len);
    for (i = 0; i < len; i++) {
        int hi = g_ascii_xdigit_value(comma[1 + 2 * i]);
        int lo = g_ascii_xdigit_value(comma[2 + 2 * i]);

        if (hi < 0 || lo < 0) {
            error_setg(errp, "bad hex data");
            return;
        }
        buf[i] = hi << 4 | lo;
    }
    if (address_space_write(&s->as, addr, MEMTXATTRS_UNSPECIFIED, buf, len) !=
        MEMTX_OK) {
        error_setg(errp, "write to the x86 address space failed");
    }
}

/* Write-only "peek" property: "ADDR,LEN" (hex ADDR, hex LEN <= 4096) */
static void psp_x86_set_peek(Object* obj, const char* str, Error** errp)
{
    PSPX86State* s = PSP_X86(obj);
    const char* comma = strchr(str, ',');
    g_autofree char* astr = NULL;
    g_autofree uint8_t* buf = NULL;
    GString* hex;
    uint64_t addr, len;

    g_clear_pointer(&s->peek_result, g_free);
    if (!comma) {
        error_setg(errp, "expected \"ADDR,LEN\"");
        return;
    }
    astr = g_strndup(str, comma - str);
    if (qemu_strtou64(astr, NULL, 16, &addr) < 0 ||
        qemu_strtou64(comma + 1, NULL, 16, &len) < 0) {
        error_setg(errp, "expected hex \"ADDR,LEN\"");
        return;
    }
    if (len == 0 || len > X86_POKE_MAX) {
        error_setg(errp, "LEN must be 1..%d", X86_POKE_MAX);
        return;
    }
    if (addr >= X86_PHYS_ADDR_SPACE_SIZE ||
        len > X86_PHYS_ADDR_SPACE_SIZE - addr) {
        error_setg(errp, "range outside the x86 address space");
        return;
    }
    buf = g_malloc(len);
    if (address_space_read(&s->as, addr, MEMTXATTRS_UNSPECIFIED, buf, len) !=
        MEMTX_OK) {
        error_setg(errp, "read from the x86 address space failed");
        return;
    }
    hex = g_string_sized_new(2 * len);
    for (uint64_t i = 0; i < len; i++) {
        g_string_append_printf(hex, "%02x", buf[i]);
    }
    s->peek_result = g_string_free(hex, false);
}

static char* psp_x86_get_peek_result(Object* obj, Error** errp)
{
    PSPX86State* s = PSP_X86(obj);

    return g_strdup(s->peek_result ? s->peek_result : "");
}

static void psp_x86_init(Object* obj)
{
    PSPX86State* s = PSP_X86(obj);
    object_initialize_child(obj, "misc", &s->psp_x86_misc, TYPE_PSP_X86_MISC);
    object_initialize_child(obj, "config_space", &s->pci_config_space,
                            TYPE_PSP_X86_CONFIG_SPACE);
    object_initialize_child(obj, "psp-x86-sts", &s->sts, TYPE_PSP_STS);
}
/* Upper bound of the regular DRAM block */
#define REGULAR_DRAM_SIZE UINT64_C(0x200000000)
/* Default sizes, used when 'regular_dram_size' is left at 0 */
#define REGULAR_DRAM_DEFAULT_SIZE UINT64_C(0x40000000)
#define REGULAR_DRAM_ZEN3_DEFAULT_SIZE UINT64_C(0x200000000)
#define REGULAR_DRAM_BASE 0x0
/*
 * DRAM beyond this size is not mapped contiguously from 0: like on a real PC
 * the first REGULAR_DRAM_LOW_SIZE bytes sit below the MMIO hole and the rest
 * above 4 GiB, so that the x86 MMIO windows below 4 GiB stay reachable.
 */
#define REGULAR_DRAM_LOW_SIZE UINT64_C(0x80000000)
#define REGULAR_DRAM_HIGH_BASE UINT64_C(0x100000000)

#define PROTECTED_REGION_BASE 0xfffd00000000UL

#define SECURE_DRAM_BASE (PROTECTED_REGION_BASE + 0xfb000000)
/* 12.5MiB*/
#define SECURE_DRAM_SIZE (125 * 1024 * 1024 / 10)

#define MMIO_BASE (PROTECTED_REGION_BASE + 0xfc000000)
#define MASTER_STATUS_PORT_BASE (MMIO_BASE + 0x80)

#define FCH_UART0_BASE 0xfedc9000UL

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
    if (s->regular_dram_size == 0) {
        s->regular_dram_size = s->generation == ZEN3 ?
            REGULAR_DRAM_ZEN3_DEFAULT_SIZE : REGULAR_DRAM_DEFAULT_SIZE;
    }
    if (s->regular_dram_size > REGULAR_DRAM_SIZE ||
        !QEMU_IS_ALIGNED(s->regular_dram_size, qemu_real_host_page_size())) {
        error_setg(errp,
                   "property 'regular_dram_size' must be a page-aligned size "
                   "in (0, 0x%" PRIx64 "]", REGULAR_DRAM_SIZE);
        return;
    }
    memory_region_init_ram(&s->regular_dram, OBJECT(dev), "x86.regular_dram",
                           s->regular_dram_size, &error_abort);
    if (s->regular_dram_size <= REGULAR_DRAM_LOW_SIZE) {
        memory_region_add_subregion(&s->mr, REGULAR_DRAM_BASE,
                                    &s->regular_dram);
    } else {
        /* One RAM block (one snapshot unit), exposed through two aliases */
        memory_region_init_alias(&s->regular_dram_low, OBJECT(dev),
                                 "x86.regular_dram_low", &s->regular_dram, 0,
                                 REGULAR_DRAM_LOW_SIZE);
        memory_region_add_subregion(&s->mr, REGULAR_DRAM_BASE,
                                    &s->regular_dram_low);
        memory_region_init_alias(&s->regular_dram_high, OBJECT(dev),
                                 "x86.regular_dram_high", &s->regular_dram,
                                 REGULAR_DRAM_LOW_SIZE,
                                 s->regular_dram_size - REGULAR_DRAM_LOW_SIZE);
        memory_region_add_subregion(&s->mr, REGULAR_DRAM_HIGH_BASE,
                                    &s->regular_dram_high);
    }

    /* Map PSP Status port */
    qdev_prop_set_bit(DEVICE(&s->sts), "in_x86_space", true);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sts), errp)) {
        return;
    }
    MemoryRegion* mr_status =
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->sts), 0);
    memory_region_add_subregion(&s->mr, MASTER_STATUS_PORT_BASE, mr_status);

    /*
     * FCH UART 0 (UART at offset 0x9000 of the 0xfedc0000 block): a 16550
     * with 4-byte register stride. Output goes to the first -serial
     * chardev, or is dropped when none is attached.
     */
    serial_mm_init(&s->mr, FCH_UART0_BASE, 2, NULL, 115200, serial_hd(0),
                   DEVICE_LITTLE_ENDIAN);

    /* The Zen3 off-chip BL's TPM bring-up polls a TIS interface */
    if (s->generation == ZEN3) {
        object_initialize_child(OBJECT(dev), "tpm", &s->tpm,
                                TYPE_PSP_X86_TPM);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->tpm), errp)) {
            return;
        }
        memory_region_add_subregion(
            &s->mr, PSP_X86_TPM_BASE,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->tpm), 0));
    }

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
    DEFINE_PROP_UINT64("regular_dram_size", PSPX86State, regular_dram_size, 0),
};

static void psp_x86_class_init(ObjectClass* oc, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    dc->realize = psp_x86_realize;
    device_class_set_props(dc, properties);
    object_class_property_add_str(oc, "poke", NULL, psp_x86_set_poke);
    object_class_property_add_str(oc, "peek", NULL, psp_x86_set_peek);
    object_class_property_add_str(oc, "peek_result", psp_x86_get_peek_result,
                                  NULL);
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
