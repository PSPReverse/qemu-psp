/*
 * AMD PSP Zen3 SEV firmware mailbox.
 *
 * The Zen3 off-chip BL takes SEV commands from the host (inferred from static
 * analysis, not documentation): the host writes the x86 buffer address to
 * 0x030109e0 (low) and 0x030109e4 (high), the command as cmd << 16 (bit 31
 * clear) to 0x03010980 and pends interrupt controller source 0x40. The BL
 * hands the command to the SEV applet and answers by writing
 * 0x80000000 | (cmd & ~0xffff) | ret back to 0x03010980; it also writes
 * 0x80000000 there at init as "ready".
 *
 * The three words read and write back. A command write from outside the
 * guest raises the IC source and leaves the line high until the BL
 * acknowledges it, like the host mailbox doorbell.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"

#include "psp/mmio/sev-mailbox.h"

#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/main-loop.h"
#include "system/address-spaces.h"

#define SEV_READY_BIT 0x80000000u

typedef struct SevMailboxWord {
    SevMailboxState* s;
    unsigned int index;
} SevMailboxWord;

static const hwaddr sev_mailbox_addr[SEV_MAILBOX_WORDS] = {
    PSP_SEV_MAILBOX_CMD, PSP_SEV_MAILBOX_ADDR_LO, PSP_SEV_MAILBOX_ADDR_HI,
};

static SevMailboxWord sev_mailbox_words[SEV_MAILBOX_WORDS];

static uint64_t sev_mailbox_read(void* opaque, hwaddr offset, unsigned size)
{
    SevMailboxWord* w = opaque;

    return extract32(w->s->values[w->index], (offset & 3) * 8, size * 8);
}

static MemTxResult sev_mailbox_write(void* opaque, hwaddr offset,
                                     uint64_t value, unsigned size,
                                     MemTxAttrs attrs)
{
    SevMailboxWord* w = opaque;
    SevMailboxState* s = w->s;

    s->values[w->index] = deposit32(s->values[w->index], (offset & 3) * 8,
                                    size * 8, value);
    /*
     * Writes from outside the guest carry MEMTXATTRS_UNSPECIFIED; the PSP
     * core and the CCP carry explicit attributes and never ring, whatever
     * they write. A word with bit 31 set is a reply, not a command.
     */
    if (w->index == 0 && s->doorbell_irq_id >= 0 && attrs.unspecified &&
        !(s->values[0] & SEV_READY_BIT)) {
        qemu_set_irq(s->doorbell_irq, 1);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps sev_mailbox_ops = {
    .read = sev_mailbox_read,
    .write_with_attrs = sev_mailbox_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void sev_mailbox_reset_hold(Object* obj, ResetType type)
{
    SevMailboxState* s = SEV_MAILBOX(obj);

    memset(s->values, 0, sizeof(s->values));
}

static const VMStateDescription sev_mailbox_vmstate = {
    .name = TYPE_SEV_MAILBOX,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(values, SevMailboxState, SEV_MAILBOX_WORDS),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * Fuzzer hook: write word 0..2 as the host side would, through the PSP
 * address space with unspecified attributes, so the doorbell sees an external
 * write. The BQL is taken if the caller does not hold it. Returns -1 if no
 * SEV mailbox is realized or word >= 3, else 0 or the MemTxResult of the write
 * (non-zero on failure).
 */
static SevMailboxState* aspfuzz_sev_mailbox_state;

static int32_t sev_mailbox_host_write(SevMailboxState* s, uint32_t word,
                                      uint32_t value)
{
    bool took_bql = !bql_locked();
    int32_t rc = -1;

    if (took_bql) {
        bql_lock();
    }
    if (s != NULL && word < SEV_MAILBOX_WORDS) {
        rc = address_space_write(&address_space_memory,
                                 sev_mailbox_addr[word],
                                 MEMTXATTRS_UNSPECIFIED, &value, 4);
    }
    if (took_bql) {
        bql_unlock();
    }
    return rc;
}

int32_t aspfuzz_sev_mailbox_write(uint32_t word, uint32_t value);
int32_t aspfuzz_sev_mailbox_write(uint32_t word, uint32_t value)
{
    return sev_mailbox_host_write(aspfuzz_sev_mailbox_state, word, value);
}

/*
 * Fuzzer hook: read word 0..2 straight from the device state (no bus
 * access, no side effects). The BQL is taken if the caller does not hold it.
 * Returns -1 if no SEV mailbox is realized or word >= 3, else 0.
 */
int32_t aspfuzz_sev_mailbox_read(uint32_t word, uint32_t* value);
int32_t aspfuzz_sev_mailbox_read(uint32_t word, uint32_t* value)
{
    SevMailboxState* s = aspfuzz_sev_mailbox_state;
    bool took_bql = !bql_locked();
    int32_t rc = -1;

    if (took_bql) {
        bql_lock();
    }
    if (s != NULL && value != NULL && word < SEV_MAILBOX_WORDS) {
        *value = s->values[word];
        rc = 0;
    }
    if (took_bql) {
        bql_unlock();
    }
    return rc;
}

/* Read-only "words" property: "w0,w1,w2" in hex */
static char* sev_mailbox_get_words(Object* obj, Error** errp)
{
    SevMailboxState* s = SEV_MAILBOX(obj);

    return g_strdup_printf("0x%x,0x%x,0x%x", s->values[0], s->values[1],
                           s->values[2]);
}

/* Write-only "write" property: "WORD,VALUE", strtoull base 0 */
static void sev_mailbox_set_write(Object* obj, const char* str, Error** errp)
{
    SevMailboxState* s = SEV_MAILBOX(obj);
    const char* comma = strchr(str, ',');
    g_autofree char* wstr = NULL;
    uint64_t word, value;
    int32_t rc;

    if (!comma) {
        error_setg(errp, "expected \"WORD,VALUE\"");
        return;
    }
    wstr = g_strndup(str, comma - str);
    if (qemu_strtou64(wstr, NULL, 0, &word) < 0 ||
        qemu_strtou64(comma + 1, NULL, 0, &value) < 0) {
        error_setg(errp, "expected \"WORD,VALUE\"");
        return;
    }
    if (word >= SEV_MAILBOX_WORDS) {
        error_setg(errp, "SEV mailbox word must be 0..2");
        return;
    }
    if (value > UINT32_MAX) {
        error_setg(errp, "value does not fit in 32 bits");
        return;
    }
    rc = sev_mailbox_host_write(s, word, value);
    if (rc != 0) {
        error_setg(errp, "SEV mailbox write failed (%d)", rc);
    }
}

static void sev_mailbox_unrealize(DeviceState* dev)
{
    if (aspfuzz_sev_mailbox_state == SEV_MAILBOX(dev)) {
        aspfuzz_sev_mailbox_state = NULL;
    }
}

static void sev_mailbox_realize(DeviceState* dev, Error** errp)
{
    SevMailboxState* s = SEV_MAILBOX(dev);
    SysBusDevice* sbd = SYS_BUS_DEVICE(dev);

    if (s->doorbell_irq_id < -1 || s->doorbell_irq_id >= 128) {
        error_setg(errp, "doorbell_irq_id must be -1 or below 128");
        return;
    }

    for (unsigned int i = 0; i < SEV_MAILBOX_WORDS; i++) {
        sev_mailbox_words[i] = (SevMailboxWord){ s, i };
        memory_region_init_io(&s->iomem[i], OBJECT(dev), &sev_mailbox_ops,
                              &sev_mailbox_words[i], TYPE_SEV_MAILBOX, 4);
        sysbus_init_mmio(sbd, &s->iomem[i]);
    }
    sysbus_init_irq(sbd, &s->doorbell_irq);
    aspfuzz_sev_mailbox_state = s;
}

static const Property sev_mailbox_properties[] = {
    DEFINE_PROP_INT32("doorbell_irq_id", SevMailboxState, doorbell_irq_id,
                      SEV_MAILBOX_DOORBELL_DEFAULT),
};

static void sev_mailbox_class_init(ObjectClass* oc, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    ResettableClass* rc = RESETTABLE_CLASS(oc);

    dc->realize = sev_mailbox_realize;
    dc->unrealize = sev_mailbox_unrealize;
    dc->vmsd = &sev_mailbox_vmstate;
    rc->phases.hold = sev_mailbox_reset_hold;
    device_class_set_props(dc, sev_mailbox_properties);
    object_class_property_add_str(oc, "write", NULL, sev_mailbox_set_write);
    object_class_property_add_str(oc, "words", sev_mailbox_get_words, NULL);
}

static const TypeInfo sev_mailbox_info = {
    .name = TYPE_SEV_MAILBOX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .class_init = sev_mailbox_class_init,
    .instance_size = sizeof(SevMailboxState),
};

static void sev_mailbox_register_types(void)
{
    type_register_static(&sev_mailbox_info);
}
type_init(sev_mailbox_register_types);
