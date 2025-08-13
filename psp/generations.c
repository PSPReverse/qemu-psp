#include "qemu/osdep.h"

#include "psp/generations.h"

#include "qemu/log.h"

const char* amd_psp_gen_ident[] = {
    [ZEN] = "Ryzen Zen",
    [ZEN_PLUS] = "Ryzen Zen Plus",
    [ZEN2] = "Ryzen Zen 2",
    [ZEN3] = "Ryzen Zen 3",
    [ZENTESLA] = "Ryzen Zen Tesla",
    [PSP_GENERATION_MAX] = "INVALID",
};

PspGeneration PspNameToGen(const char* name)
{
    for (int i = 0; i <= ZENTESLA; ++i) {
        if (strcmp(name, amd_psp_gen_ident[i]) == 0) {
            return i;
        }
    }
    qemu_log_mask(LOG_GUEST_ERROR, "Invalid PspNameToGen\n");
    exit(1);
}

const char* PspGenToName(PspGeneration gen)
{
    if (gen < PSP_GENERATION_MAX) {
        return amd_psp_gen_ident[gen];
    }
    qemu_log_mask(LOG_GUEST_ERROR, "Invalid PspGenToName\n");
    exit(1);
}
