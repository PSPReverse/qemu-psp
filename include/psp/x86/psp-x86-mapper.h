/*
 * Copyright (C) 2024 Stefan Zabka <stefan@zabka.it>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PSP_X86_ACCESS_H
#define PSP_X86_ACCESS_H
#include "qemu/osdep.h"

#include "exec/memory.h"
#include "hw/sysbus.h"

#include "psp/psp-misc.h"
#include "psp/x86/psp-x86.h"

#define PSP_X86_MAPPER_NAME "PSP X86 Mapper"
#define PSP_X86_MAPPING_BASE 0x04000000
#define PSP_X86_CTRL1_BASE 0x03230000
#define PSP_X86_MAGIC1_BASE 0x032303e0
#define PSP_X86_MAGIC2_BASE 0x032304d8
#define PSP_X86_MAGIC3_BASE 0x032305ec

#define TYPE_PSP_X86_MAPPER "amd_psp.x86.mapper"
#define PSP_X86_MAPPER(obj)                                                    \
    OBJECT_CHECK(PSPX86MapperState, (obj), TYPE_PSP_X86_MAPPER)
#define PSP_X86_SLOT_NAME_LEN 20
#define PSP_X86_SLOT_NAME "x86-slot_"

#define PSP_X86_SLOT_SIZE 64 * 1024 * 1024 /* 64M */
#define PSP_X86_SLOT_COUNT 62
#define PSP_X86_CONTAINER_SIZE PSP_X86_SLOT_SIZE* PSP_X86_SLOT_COUNT
#define PSP_X86_REG_COUNT 6

#define PSP_X86_CTLR_SIZE PSP_X86_SLOT_COUNT * 4 * 4
#define PSP_X86_MAGIC1_SIZE PSP_X86_SLOT_COUNT * 4
#define PSP_X86_MAGIC2_SIZE PSP_X86_MAGIC1_SIZE
#define PSP_X86_MAGIC3_SIZE 4

typedef enum PSPX86RegId {
    X86BASE = 0,
    X86_UNKNOWN1,
    X86_UNKNOWN2,
    X86_UNKNOWN3,
    X86_UNKNOWN4,
    X86_UNKNOWN5,
    X86_REG_MAX = PSP_X86_REG_COUNT,
} PSPX86RegId;

typedef uint64_t PSPX86Addr;
/**
 * @brief Contains information about a single x86 container mapping
 *
 * This information is spread over multiple control registers in the hardwar
 * but we store it together for convinience
 */
typedef struct PSPX86Slot {

    /* Currently mapped x86 address */
    PSPX86Addr x86_addr;

    /* Current mapping destination */
    hwaddr psp_addr;

    uint32_t ctrl_regs[PSP_X86_REG_COUNT];
} PSPX86Slot;

/**
 * @brief The Magic3 register is a single register
 * of unkown purpose
 *
 * It gets set to 0x3333 at least once on x86 container enable
 * but never reset as far as we know.
 * This state tracks the state of the register and how often
 * it has been set to 0x3333
 */
typedef struct Magic3State {
    uint32_t value;
    uint32_t counter;
} Magic3State;

/**
 * @brief Handles the x86 container mapping and control registers
 *
 * Also owns the PSPX86State that controls the underlying address space
 *
 */
typedef struct PSPX86MapperState {
    SysBusDevice parent_obj;

    /* X86 Control regs */

    /* CTRL region 1: PSP_X86_SLOT_COUNT * regsize (4) * regcount (4) = 0x3e0
     * Contains the actual target address of the x86 container
     */
    MemoryRegion psp_x86_control;

    /* CTRL region 2: PSP_X86_SLOT_COUNT * regsize (4) * regcount (1) = 0xf8
     * Gets set to 0XFFFFFFFF on x86 container enable and disable
     * (Disable might not be required)
     */
    MemoryRegion psp_x86_magic1;

    /* CTRL region 3: PSP_X86_SLOT_COUNT * regsize (4) * regcount (1) = 0xf8
     * Set to 0xc0000000 on x86 container enable and 0 on disable
     */
    MemoryRegion psp_x86_magic2;

    /* Single register: Set to 0x3333  */
    MemoryRegion psp_x86_magic3;
    Magic3State magic3;

    /**
     * @brief The memory regions that represent the x86 containers
     *
     * They are themselves aliased into the PSP address space
     * but also alias regions in the x86 address space
     *
     * This second alias gets updated whenever an appropriate write
     * happens to the control registers
     *
     */
    MemoryRegion psp_x86_containers[PSP_X86_SLOT_COUNT];

    PSPX86Slot psp_x86_slots[PSP_X86_SLOT_COUNT];

    /* Manages the underlying address space and all devices mounted into it*/
    PSPX86State x86_memory;

    /* The Generation of PSP we are emulating*/
    PspGeneration generation;

} PSPX86MapperState;
#endif
