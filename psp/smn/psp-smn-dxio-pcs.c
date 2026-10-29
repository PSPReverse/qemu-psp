/*
 * AMD PSP Zen3 DXIO PCS state registers.
 *
 * The DXIO firmware phase of the ABL drives the PCS instances 0x12e..0x133
 * (SMN base idx << 20) through a command/state register pair (derived from
 * the access sequence of the caller, not from documentation):
 *   +0x40 command; a write is a trigger that completes at once, so it reads 0
 *   +0x78 state history; the low byte is the last command written
 * The other registers of the window read 0 and ignore writes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"

#include "psp/smn/psp-smn-dxio-pcs.h"

#include "hw/resettable.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "trace.h"

#define REG_CMD 0x40
#define REG_STATE_HIST 0x78

static uint64_t psp_dxio_pcs_read(void *opaque, hwaddr offset, unsigned size)
{
    PSPSmnDxioPcsInst *inst = opaque;
    uint32_t val = 0;

    if ((offset & ~3) == REG_STATE_HIST) {
        val = inst->parent->state_hist[inst->index] >> ((offset & 3) * 8);
    } else if ((offset & ~3) != REG_CMD) {
        qemu_log_mask(LOG_UNIMP, "psp-smn-dxio-pcs: read of unknown "
                      "register 0x%" HWADDR_PRIx " of instance 0x%x\n",
                      offset, PSP_SMN_DXIO_PCS_FIRST + inst->index);
    }
    trace_psp_smn_dxio_pcs_read(inst->index, offset, val);
    return val & MAKE_64BIT_MASK(0, size * 8);
}

static void psp_dxio_pcs_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    PSPSmnDxioPcsInst *inst = opaque;

    trace_psp_smn_dxio_pcs_write(inst->index, offset, value);
    if ((offset & ~3) == REG_CMD) {
        inst->parent->state_hist[inst->index] = value;
    }
}

static const MemoryRegionOps psp_dxio_pcs_ops = {
    .read = psp_dxio_pcs_read,
    .write = psp_dxio_pcs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void psp_dxio_pcs_init(Object *obj)
{
    PSPSmnDxioPcsState *s = PSP_SMN_DXIO_PCS(obj);

    for (unsigned i = 0; i < PSP_SMN_DXIO_PCS_COUNT; i++) {
        PSPSmnDxioPcsInst *inst = &s->inst[i];

        inst->parent = s;
        inst->index = i;
        memory_region_init_io(&inst->iomem, obj, &psp_dxio_pcs_ops, inst,
                              "psp-smn-dxio-pcs", PSP_SMN_DXIO_PCS_SIZE);
        sysbus_init_mmio(SYS_BUS_DEVICE(obj), &inst->iomem);
    }
}

static void psp_dxio_pcs_reset_hold(Object *obj, ResetType type)
{
    PSPSmnDxioPcsState *s = PSP_SMN_DXIO_PCS(obj);

    memset(s->state_hist, 0, sizeof(s->state_hist));
}

static const VMStateDescription psp_dxio_pcs_vmstate = {
    .name = "psp-smn-dxio-pcs",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(state_hist, PSPSmnDxioPcsState,
                             PSP_SMN_DXIO_PCS_COUNT),
        VMSTATE_END_OF_LIST()
    }
};

static void psp_dxio_pcs_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->vmsd = &psp_dxio_pcs_vmstate;
    rc->phases.hold = psp_dxio_pcs_reset_hold;
}

static const TypeInfo psp_dxio_pcs_info = {
    .name = TYPE_PSP_SMN_DXIO_PCS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PSPSmnDxioPcsState),
    .instance_init = psp_dxio_pcs_init,
    .class_init = psp_dxio_pcs_class_init,
};

static void psp_dxio_pcs_register_types(void)
{
    type_register_static(&psp_dxio_pcs_info);
}

type_init(psp_dxio_pcs_register_types)
