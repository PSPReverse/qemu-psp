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
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "qemu/log.h"

#include "trace.h"
#include "psp/smn/psp-smn-smu.h"


static uint64_t psp_smn_smu_read(void *opaque, hwaddr offset, unsigned size) {
    PSPSmnSmuState *s = PSP_SMN_SMU(opaque);

    if (size != sizeof(uint8_t)) {
        qemu_log_mask(LOG_UNIMP,
                      "PSP SMN SMU: Error. Unsupported read size\n");
        return 0;
    }

    if (offset >= PSP_SMN_SMU_FW_SIZE) {
        qemu_log_mask(LOG_UNIMP,
                      "PSP SMN SMU: Error. Offset %#lx too large.\n",
                      offset);
        return 0;
    }

    return s->fw_buf[offset];
}

static void psp_smn_smu_write(void *opaque, hwaddr offset, uint64_t val, unsigned size) {
    PSPSmnSmuState *s = PSP_SMN_SMU(opaque);

    if (size != sizeof(uint8_t)) {
        qemu_log_mask(LOG_UNIMP,
                      "PSP SMN SMU: Error. Unsupported read size\n");
        return;
    }

    if (offset >= PSP_SMN_SMU_FW_SIZE) {
        qemu_log_mask(LOG_UNIMP,
                      "PSP SMN SMU: Error. Offset %#lx too large.\n",
                      offset);
        return;
    }

    s->fw_buf[offset] = val;
}

static const MemoryRegionOps psp_smn_smu_mem_ops = {
    .read = psp_smn_smu_read,
    .write = psp_smn_smu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static void psp_smn_smu_init(Object *obj)
{
    PSPSmnSmuState *s = PSP_SMN_SMU(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->psp_smn_smu, obj, &psp_smn_smu_mem_ops, s,
                           TYPE_PSP_SMN_SMU, PSP_SMN_SMU_FW_SIZE);
    sysbus_init_mmio(sbd, &s->psp_smn_smu);
}

static const TypeInfo psp_smn_smu_info = {
    .name = TYPE_PSP_SMN_SMU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_init = psp_smn_smu_init,
    .instance_size = sizeof(PSPSmnSmuState),
};

static void psp_smn_register_types(void) {
    type_register_static(&psp_smn_smu_info);
}
type_init(psp_smn_register_types);
