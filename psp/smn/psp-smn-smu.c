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

#include "psp/smn/psp-smn-smu.h"

#include "qapi/error.h"

static void psp_smn_smu_init(Object* obj)
{
    PSPSmnSmuState* s = PSP_SMN_SMU(obj);
    SysBusDevice* sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_ram(&s->psp_smn_smu, obj, TYPE_PSP_SMN_SMU,
                           PSP_SMN_SMU_FW_SIZE, &error_abort);
    sysbus_init_mmio(sbd, &s->psp_smn_smu);
}

static const TypeInfo psp_smn_smu_info = {
    .name = TYPE_PSP_SMN_SMU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_init = psp_smn_smu_init,
    .instance_size = sizeof(PSPSmnSmuState),
};

static void psp_smn_register_types(void)
{
    type_register_static(&psp_smn_smu_info);
}
type_init(psp_smn_register_types);
