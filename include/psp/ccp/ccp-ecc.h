/*
 * AMD PSP CCP v5 ECC engine arithmetic
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef AMD_CCP_ECC_H
#define AMD_CCP_ECC_H

#include "qemu/osdep.h"

/* The engine takes 8 little-endian slots, 0x30 bytes of each are used */
#define CCP_ECC_SLOT_SIZE 0x48
#define CCP_ECC_NUM_SLOTS 8
#define CCP_ECC_SRC_SIZE (CCP_ECC_SLOT_SIZE * CCP_ECC_NUM_SLOTS)
#define CCP_ECC_WIDTH 0x30
/* PMUL writes the affine y after x, at this offset */
#define CCP_ECC_Y_OFFSET 0x4c
#define CCP_ECC_DST_SIZE (CCP_ECC_Y_OFFSET + CCP_ECC_SLOT_SIZE)

#define CCP_ECC_OP_MMUL 0
#define CCP_ECC_OP_MADD 1
#define CCP_ECC_OP_MINV 2
#define CCP_ECC_OP_PMUL 4
#define CCP_ECC_OP_PCOMB 6

/**
 * ccp_ecc_execute: run one ECC engine operation
 * @op: operation code
 * @src: CCP_ECC_SRC_SIZE bytes, the eight input slots
 * @dst: CCP_ECC_DST_SIZE bytes, zeroed and then filled with the result:
 *       ops 0-2 use the first CCP_ECC_SLOT_SIZE bytes, PMUL puts x at 0 and
 *       y at CCP_ECC_Y_OFFSET, as does PCOMB (A * P1 + B * P2). Undefined results (zero modulus, no inverse,
 *       point at infinity) are all zero.
 *
 * Returns false if @op is not supported, @dst is then untouched.
 */
bool ccp_ecc_execute(unsigned op, const uint8_t *src, uint8_t *dst);

#endif
