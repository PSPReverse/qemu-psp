/*
 * AMD PSP Zen3 SMBus controller (SMN 0x2d01a00) with DDR4 SPD EEPROMs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef AMD_PSP_SMN_SMBUS_H
#define AMD_PSP_SMN_SMBUS_H

#include "hw/sysbus.h"

#define TYPE_PSP_SMN_SMBUS "amd_psp.smn.smbus"
#define PSP_SMN_SMBUS(obj)                                                     \
    OBJECT_CHECK(PSPSmnSmbusState, (obj), TYPE_PSP_SMN_SMBUS)

/* SMN address of the register block */
#define PSP_SMN_SMBUS_BASE 0x2d01a00
#define PSP_SMN_SMBUS_REGS 16
#define PSP_SMN_SMBUS_SLOTS 8
#define PSP_SPD_SIZE 512

typedef struct PSPSmnSmbusState {
    /* <private> */
    SysBusDevice parent;
    /* <public> */
    MemoryRegion iomem;

    /* Properties */
    uint32_t dimm_slots;    /* bit n: DIMM at 7-bit address 0x50 + n, default slot 1 */
    char* spd_file;         /* optional 512 byte image for every DIMM */

    /* State */
    uint8_t regs[PSP_SMN_SMBUS_REGS];
    uint8_t spd_page;       /* selected by the 0x36/0x37 pseudo devices */
    uint8_t spd_ptr;        /* EEPROM address pointer within the page */

    /* Derived from the properties at realize, not migrated */
    uint8_t spd[PSP_SMN_SMBUS_SLOTS][PSP_SPD_SIZE];
} PSPSmnSmbusState;

/* JEDEC SPD CRC16 (polynomial 0x1021, initial value 0) */
uint16_t psp_spd_crc16(const uint8_t* data, size_t len);

#endif
