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
#ifndef AMD_PSP_SMN_H
#define AMD_PSP_SMN_H

#include "system/memory.h"
#include "hw/sysbus.h"

#include "psp/psp-window.h"
#include "psp/smn/psp-smn-flash.h"
#include "psp/smn/psp-smn-misc.h"
#include "psp/smn/psp-smn-mp5.h"
#include "psp/smn/psp-smn-dxio-pcs.h"
#include "psp/smn/psp-smn-smbus.h"
#include "psp/smn/psp-smn-smu.h"
#include "psp/smn/psp-smn-umc.h"

#define PSP_SMN_CTRL_SIZE 16 * 4
#define PSP_SMN_SLOT_SIZE 1024 * 1024 /* 1M */
#define PSP_SMN_SLOT_COUNT 32
#define PSP_SMN_SLOT_NAME_LEN 20
#define PSP_SMN_SLOT_NAME "smn-slot_"

#define TYPE_PSP_SMN "amd_psp.smn"
#define TYPE_PSP_SMN_ZEN "amd_psp.smn.zen"
#define TYPE_PSP_SMN_ZEN_PLUS "amd_psp.smn.zen_plus"
#define TYPE_PSP_SMN_ZEN2 "amd_psp.smn.zen2"
#define TYPE_PSP_SMN_ZEN3 "amd_psp.smn.zen3"
#define TYPE_PSP_SMN_ZENTESLA "amd_psp.smn.zentesla"
#define PSP_SMN(obj) OBJECT_CHECK(PSPSmnState, (obj), TYPE_PSP_SMN)

typedef uint32_t PSPSmnAddr;

#define PSP_SMN_MAX_RAM_WINDOWS 4

typedef struct PSPSmnState {
    SysBusDevice parent_obj;

    /* MemoryRegion containing the MMIO control registers */
    MemoryRegion psp_smn_control;

    /* The SMN slots */
    PspWindow psp_smn_windows[PSP_SMN_SLOT_COUNT];

    /* Full 32bit SMN address space */
    MemoryRegion psp_smn_space;
    /* Full 32bit SMN address space */
    AddressSpace psp_smn_space_as;
    /* Second window onto the flash image (Zen3 off-chip BL reads 0x47000000) */
    MemoryRegion psp_smn_flash_alias;

    /* PSP Misc device that covers the whole SMN address space */
    // PSPMiscState psp_smn_misc;
    PSPSmnMiscState psp_smn_misc;

    /* Base address of the smn range. */
    hwaddr psp_smn_base;

    /* Current SMN state */
    PSPSmnAddr psp_smn_slots[32];
    /*
     * Offset the containers are aliased at. Equals psp_smn_slots[] once the
     * guest has programmed a slot, the power-on offset before that.
     */
    hwaddr psp_smn_alias[PSP_SMN_SLOT_COUNT];

    /* Zen3 SMBus controller with the DIMM SPD EEPROMs */
    PSPSmnSmbusState psp_smn_smbus;

    /* Zen3 DXIO PCS state registers */
    PSPSmnDxioPcsState psp_smn_dxio_pcs;

    /* Zen3 SMU message mailbox */
    PSPSmnSmuState psp_smn_smu;

    /* Zen3 MP5 stand-in */
    PSPSmnMp5State psp_smn_mp5;

    /* Zen3 UMC channel APB windows with the PMU mailbox */
    PSPSmnUmcState psp_smn_umc;

    /* The SMN attached flash */
    PSPSmnFlashState psp_smn_flash;

    /* RAM windows of the SMN address space, see psp/regs */
    MemoryRegion psp_smn_ram[PSP_SMN_MAX_RAM_WINDOWS];

} PSPSmnState;
#endif
