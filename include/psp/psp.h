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
#ifndef AMD_PSP_H
#define AMD_PSP_H

#include "hw/arm/boot.h"
#include "hw/misc/unimp.h"
#include "hw/sysbus.h"
#include "target/arm/cpu.h"

#include "psp/ccp/ccpv5.h"
#include "psp/generations.h"
#include "psp/mmio/mailbox.h"
#include "psp/mmio/psp-fuse.h"
#include "psp/mmio/psp-sts.h"
#include "psp/mmio/psp-timer.h"
#include "psp/psp-misc.h"
#include "psp/smn/psp-smn.h"
#include "psp/x86/psp-x86-mapper.h"

#define TYPE_AMD_PSP "amd-psp"
#define AMD_PSP(obj) OBJECT_CHECK(AmdPspState, (obj), TYPE_AMD_PSP)

#define PSP_SMN_CTRL_BASE 0x03220000

#define PSP_TIMER1_BASE 0x03010400
#define PSP_TIMER2_BASE 0x03010424

#define PSP_STS_ZEN1_BASE 0x032000e8

#define PSP_CCP_BASE 0x03000000

#define PSP_SMN_BASE 0x01000000
#define PSP_SMN_NAME "PSP SMN"

#define PSP_MMIO_BASE 0x03000000
#define PSP_MMIO_NAME "PSP MMIO"

// TODO: check if this is consistent across versions
#define PSP_MAILBOX_BASE 0x03010570

#define PSP_FUSE_BASE 0x03010104
#define PSP_FUSE_NAME "PSP Fuse"

#define PSP_UNKNOWN_BASE 0xfc000000
#define PSP_UNKNOWN_NAME "PSP UNKNOWN"

#define PSP_ROM_BASE 0xffff0000
#define PSP_ROM_NAME "PSP ROM"

typedef struct AmdPspState {
    /*< private >*/
    DeviceState parent_obj;
    /*< public >*/

    MemoryRegion sram;
    MemoryRegion rom;

    char const* gen_ident;
    PspGeneration gen;

    bool dbg_mode;
    ARMCPU cpu;

    /* This device covers every MMIO address we have not covered somewhere else
     */
    PSPMiscState base_mem;

    /* This device represents the SMN address space including control registers
     */
    PSPSmnState smn;

    /*** Manages the x86 address space and the MMIO registers and memory
     *  containers that map into it
     */
    PSPX86MapperState x86;

    /* PSP Timer 1 at 0x03010400*/
    PSPTimerState timer1;

    /* PSP Timer 2 at 0x03010424*/
    PSPTimerState timer2;

    /* PSP Status port */
    PSPStsState sts;

    /* CCP */
    CcpV5State ccp;

    /* Fuse */
    PSPFuseState fuse;

    /* Mailbox */
    MailboxState mailbox;

    /* General unimplemented devices */
    UnimplementedDeviceState unimp;

} AmdPspState;

#endif
