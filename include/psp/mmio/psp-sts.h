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
#ifndef AMD_PSP_STS_H
#define AMD_PSP_STS_H

#include "hw/sysbus.h"

#define TYPE_PSP_STS "amd_psp.sts"
#define PSP_STS(obj) OBJECT_CHECK(PSPStsState, (obj), TYPE_PSP_STS)

/**
 * @brief The ASP Status register
 * This is used to communicate the ASP status to the host
 *
 * This register exists in two places
 * 1. In the ASP MMIO Space
 * 2. In the Secure DRAM, where it is only written by the master ASP
 *
 * To differentiate the two places the property "in_x86_space" is used
 */
typedef struct PSPStsState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    uint32_t psp_sts_val;

    bool in_x86_space;

} PSPStsState;

#endif
