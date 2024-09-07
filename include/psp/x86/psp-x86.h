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
#ifndef AMD_PSP_X86_H
#define AMD_PSP_X86_H
#include "qemu/osdep.h"

#include "exec/memory.h"
#include "hw/sysbus.h"

#include "psp/psp-misc.h"
#include "psp/x86/x86-misc.h"

#define TYPE_PSP_X86 "amd_psp.x86"
#define PSP_X86(obj) OBJECT_CHECK(PSPX86State, (obj), TYPE_PSP_X86)

/***
 * Manages the x86 address space and all x86 related devices
 */
typedef struct PSPX86State {
    SysBusDevice parent_obj;

    /* PSP Misc device that covers the whole x86 address space */
    PSPX86MiscState psp_x86_misc;

    /* The X86 address space */
    MemoryRegion psp_x86_space;

    /* The Generation of PSP we are emulating*/
    PspGeneration generation;
} PSPX86State;
#endif
