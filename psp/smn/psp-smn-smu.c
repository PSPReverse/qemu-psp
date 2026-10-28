/*
 * AMD PSP Zen3 SMU message mailbox.
 *
 * The ABL sends SMU messages through three registers at SMN 0x3b10700
 * (derived from the access sequence of the caller, not from documentation):
 *   +0x00 argument on the way in, response data on the way out
 *   +0x04 response status; 0 while a message is pending, 1 when it was
 *         processed OK (the sender clears it before the message id)
 *   +0x18 message id; the write sends the message
 * The SMU answers instantaneously on the write of the message id. The other
 * registers read/write back.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"

#include "psp/smn/psp-smn-smu.h"

#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "trace.h"

#define REG_ARG (0x00 / 4)
#define REG_RESP (0x04 / 4)
#define REG_MSG (0x18 / 4)

#define RESP_OK 1

#define MSG_QUERY_NUMBER_OF_DF_PSTATES 0x07
#define MSG_QUERY_MEM_FREQ_OF_DF_PSTATE 0x08
#define MSG_GET_STARTUP_DF_PSTATE 0x1d

#define DF_PSTATES_MAX 4

static const uint32_t mem_clk_valid[] = {
    800, 933, 1067, 1200, 1333, 1467, 1600
};

static void psp_smu_message(PSPSmnSmuState* s, uint32_t msg)
{
    uint32_t data = 0;

    switch (msg) {
    case MSG_QUERY_NUMBER_OF_DF_PSTATES:
        data = s->df_pstates;
        break;
    case MSG_QUERY_MEM_FREQ_OF_DF_PSTATE:
        /* The argument is the P-state index; all share one memory clock */
        data = s->mem_clk_mhz;
        break;
    case MSG_GET_STARTUP_DF_PSTATE:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "psp-smn-smu: unknown message 0x%x "
                      "(argument 0x%x), answering OK with 0\n",
                      msg, s->regs[REG_ARG]);
        break;
    }
    trace_psp_smn_smu_message(msg, s->regs[REG_ARG], data);
    s->regs[REG_ARG] = data;
    s->regs[REG_RESP] = RESP_OK;
}

static uint64_t psp_smu_read(void* opaque, hwaddr offset, unsigned size)
{
    PSPSmnSmuState* s = opaque;
    uint32_t val = s->regs[offset / 4];

    trace_psp_smn_smu_read(offset, val);
    return val;
}

static void psp_smu_write(void* opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    PSPSmnSmuState* s = opaque;

    trace_psp_smn_smu_write(offset, value);
    s->regs[offset / 4] = value;
    if (offset / 4 == REG_MSG) {
        psp_smu_message(s, value);
    }
}

static const MemoryRegionOps psp_smu_ops = {
    .read = psp_smu_read,
    .write = psp_smu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void psp_smu_realize(DeviceState* dev, Error** errp)
{
    PSPSmnSmuState* s = PSP_SMN_SMU(dev);

    if (s->df_pstates < 1 || s->df_pstates > DF_PSTATES_MAX) {
        error_setg(errp, "df-pstates: must be 1..%d", DF_PSTATES_MAX);
        return;
    }
    for (int i = 0; i < ARRAY_SIZE(mem_clk_valid); i++) {
        if (s->mem_clk_mhz == mem_clk_valid[i]) {
            goto mem_clk_ok;
        }
    }
    error_setg(errp, "mem-clk-mhz: must be one of 800, 933, 1067, 1200, "
               "1333, 1467, 1600");
    return;
mem_clk_ok:
    memory_region_init_io(&s->iomem, OBJECT(dev), &psp_smu_ops, s,
                          "psp-smn-smu", PSP_SMN_SMU_REGS * 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void psp_smu_reset_hold(Object* obj, ResetType type)
{
    PSPSmnSmuState* s = PSP_SMN_SMU(obj);

    memset(s->regs, 0, sizeof(s->regs));
    /* Idle: the sender waits for this before the first message */
    s->regs[REG_RESP] = RESP_OK;
}

static const VMStateDescription psp_smu_vmstate = {
    .name = "psp-smn-smu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, PSPSmnSmuState, PSP_SMN_SMU_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static const Property psp_smu_properties[] = {
    DEFINE_PROP_UINT32("df-pstates", PSPSmnSmuState, df_pstates, 1),
    DEFINE_PROP_UINT32("mem-clk-mhz", PSPSmnSmuState, mem_clk_mhz, 1200),
};

static void psp_smu_class_init(ObjectClass* oc, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    ResettableClass* rc = RESETTABLE_CLASS(oc);

    dc->realize = psp_smu_realize;
    dc->vmsd = &psp_smu_vmstate;
    rc->phases.hold = psp_smu_reset_hold;
    device_class_set_props(dc, psp_smu_properties);
}

static const TypeInfo psp_smu_info = {
    .name = TYPE_PSP_SMN_SMU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PSPSmnSmuState),
    .class_init = psp_smu_class_init,
};

static void psp_smu_register_types(void)
{
    type_register_static(&psp_smu_info);
}

type_init(psp_smu_register_types)
