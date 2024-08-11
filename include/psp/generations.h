/*
 * Copyright (C) 2024 Stefan Zabka <stefan@zabka.it>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PSP_GENERATIONS_H
#define PSP_GENERATIONS_H
/**
 * @brief Utility header that contains various identifiers for different CPU
 * generations
 *
 */

#include <stddef.h> // for _Static_assert
#include <stdint.h>

#define TYPE_AMD_PSP_ZEN "amd-psp-zen_one"
#define TYPE_AMD_PSP_ZEN_PLUS "amd-psp-zen_plus"
#define TYPE_AMD_PSP_ZEN_TWO "amd-psp-zen_two"
#define TYPE_AMD_PSP_ZEN_THREE "amd-psp-zen_three"
#define TYPE_AMD_PSP_ZEN_TESLA "amd-psp-zen_tesla"
typedef enum PspGeneration {
    ZEN = 0,
    ZEN_PLUS,
    ZEN2,
    ZEN3,
    ZENTESLA,
    PSP_GENERATION_MAX,
} PspGeneration;

_Static_assert(sizeof(PspGeneration) == sizeof(uint32_t), "PspGeneration should be uint32_t");

const char* PspGenToName(PspGeneration gen);
PspGeneration PspNameToGen(const char* name);
#endif
