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
#ifndef AMD_PSP_SMN_SMU_H
#define AMD_PSP_SMN_SMU_H

#include "hw/sysbus.h"
#include "exec/memory.h"

#define PSP_SMN_SMU_FW_SIZE 256 * 1024

#define TYPE_PSP_SMN_SMU "amd_psp.smnsmu"
#define PSP_SMN_SMU(obj) OBJECT_CHECK(PSPSmnSmuState, (obj), TYPE_PSP_SMN_SMU)

typedef struct PSPSmnSmuState {
    SysBusDevice parent_obj;

    MemoryRegion psp_smn_smu;

    char fw_buf[PSP_SMN_SMU_FW_SIZE];
} PSPSmnSmuState;

#endif
