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
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "system/system.h"
#include "exec/address-spaces.h"
#include "hw/sysbus.h"
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "psp/psp.h"
#include "qemu/log.h"
#include "trace.h"
#include "psp/mmio/mailbox.h"


static const char * index_to_name(int index) {
    switch (index)
    {
    case 0:
        return "content";
    case 1:
        return "ptr_low";
    case 2:
        return "ptr_high";
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "Invalid index_to_name\n");
        exit(1);
    }
}
static uint64_t mailbox_read(void *opaque, hwaddr offset, unsigned int size) {
    MailboxState *s = MAILBOX(opaque);
    hwaddr index = offset / 4;
    assert(index < 3);
    trace_psp_mailbox_read(index_to_name(index), s->values[index]);

    return s->values[index];
}

static void mailbox_write(void *opaque, hwaddr offset,
                       uint64_t value, unsigned int size) {
    MailboxState *s = MAILBOX(opaque);

    hwaddr index = offset / 4;
    assert(index < 3);
    s->values[index] = value;
    trace_psp_mailbox_write(index_to_name(index), s->values[index]);

}

static const MemoryRegionOps mailbox_mem_ops = {
    .read = mailbox_read,
    .write = mailbox_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void mailbox_init(Object *obj) {
    MailboxState *s = MAILBOX(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->values[0] = 0;
    s->values[1] = 0;
    s->values[2] = 0;
    memory_region_init_io(&s->iomem, obj, &mailbox_mem_ops, s,
            TYPE_MAILBOX, 4*3);

    sysbus_init_mmio(sbd, &s->iomem);

}

static const TypeInfo psp_sts_info = {
    .name = TYPE_MAILBOX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_init = mailbox_init,
    .instance_size = sizeof(MailboxState),

};

static void mailbox_register_types(void) {
    type_register_static(&psp_sts_info);

}
type_init(mailbox_register_types);
