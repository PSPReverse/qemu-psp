/*
 * AMD PSP SPI controller: command mode access to the SMN attached flash
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef AMD_PSP_SMN_SPI_H
#define AMD_PSP_SMN_SPI_H

#include "system/memory.h"
#include "hw/core/sysbus.h"

#define TYPE_PSP_SMN_SPI "amd_psp.smn.spi"
#define PSP_SMN_SPI(obj) OBJECT_CHECK(PSPSmnSpiState, (obj), TYPE_PSP_SMN_SPI)

#define PSP_SMN_SPI_BASE 0x02dc4000
#define PSP_SMN_SPI_SIZE 0x100

typedef struct PSPSmnSpiState {
    /* <private> */
    SysBusDevice parent;
    /* <public> */
    MemoryRegion iomem;
    MemoryRegion *flash;        /* backing store, set by the owner */

    /* State */
    uint8_t regs[PSP_SMN_SPI_SIZE];
    bool wel;                   /* write enable latch */
} PSPSmnSpiState;

#endif /* AMD_PSP_SMN_SPI_H */
