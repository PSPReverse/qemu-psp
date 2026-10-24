/*
 * QTest for the AMD PSP mailbox doorbell: only writes from outside the guest
 * raise it, and a CCP copy onto the mailbox is not one of them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define MACHINE "amd-psp-zen2"

#define MAILBOX_WORD0 0x03010570
#define MAILBOX_INT_ENABLE 0x0301069c
#define MAILBOX_INT_ENABLE_SECOS 0xff000000

/* Interrupt controller of Zen2 and later */
#define IC_BASE 0x03010200
#define IC_FIQ_HANDLED(group) (IC_BASE + 0xb0 + 4 * (group))
#define IC_FIQ_ALL_PROCESSED (IC_BASE + 0xc0)
#define IC_FIQ_NEXT (IC_BASE + 0xc4)
#define IC_SOURCES 128

#define SRC_CCP 0x15
#define SRC_DOORBELL 0x3c

/* CCP queue 0 and a passthrough descriptor in SRAM */
#define CCP_Q0 0x03001000
#define CCP_Q_CTRL 0x0
#define CCP_Q_HEAD 0x4
#define CCP_Q_TAIL 0x8
#define CCP_Q_STATUS 0x100
#define CCP_Q_RUN 1
#define CCP_ENGINE_PASSTHRU 5
#define CCP_MEMTYPE_LOCAL 2
#define CCP_DESC_SIZE 32
#define SRAM_DESC 0x1000
#define SRAM_SRC 0x2000

#define COMMAND 0x00010000

static char *flash_path;
static const char *skip_reason;

/* NULL, with the test marked skipped, if the machine cannot be started */
static QTestState *start(void)
{
    if (skip_reason) {
        g_test_skip(skip_reason);
        return NULL;
    }
    return qtest_initf("-machine " MACHINE
                       " -global driver=amd_psp.smnflash,property=flash_img,"
                       "value=%s",
                       flash_path);
}

/*
 * Collect the pending sources by acknowledging them one by one: the
 * controller only reports the lowest pending source.
 */
static void ic_drain(QTestState *qts, bool pending[IC_SOURCES])
{
    memset(pending, 0, IC_SOURCES * sizeof(bool));
    while (!qtest_readl(qts, IC_FIQ_ALL_PROCESSED)) {
        uint32_t src = qtest_readl(qts, IC_FIQ_NEXT);

        g_assert_cmpuint(src, <, IC_SOURCES);
        g_assert_false(pending[src]);
        pending[src] = true;
        qtest_writel(qts, IC_FIQ_HANDLED(src / 32), 1u << (src % 32));
    }
}

static void assert_only(const bool pending[IC_SOURCES], int src)
{
    for (int i = 0; i < IC_SOURCES; i++) {
        g_assert_cmpint(pending[i], ==, i == src);
    }
}

/* An external write to word 0 rings the doorbell once it is armed */
static void test_external_write_armed(void)
{
    QTestState *qts = start();
    bool pending[IC_SOURCES];

    if (!qts) {
        return;
    }
    qtest_writel(qts, MAILBOX_INT_ENABLE, MAILBOX_INT_ENABLE_SECOS);
    qtest_writel(qts, MAILBOX_WORD0, COMMAND);
    ic_drain(qts, pending);
    assert_only(pending, SRC_DOORBELL);
    qtest_quit(qts);
}

/* Without the enable register it stays silent */
static void test_external_write_unarmed(void)
{
    QTestState *qts = start();
    bool pending[IC_SOURCES];

    if (!qts) {
        return;
    }
    qtest_writel(qts, MAILBOX_INT_ENABLE, 0);
    qtest_writel(qts, MAILBOX_WORD0, COMMAND);
    ic_drain(qts, pending);
    assert_only(pending, -1);
    qtest_quit(qts);
}

/*
 * A CCP passthrough copy onto word 0 lands there, but it is CCP DMA, not an
 * external write: only the CCP completion is raised.
 */
static void test_ccp_copy(void)
{
    QTestState *qts = start();
    bool pending[IC_SOURCES];
    uint32_t desc[CCP_DESC_SIZE / 4] = {
        CCP_ENGINE_PASSTHRU << 20,  /* dw0: engine, function 0 (no-op) */
        4,                          /* length */
        SRAM_SRC,                   /* source low */
        CCP_MEMTYPE_LOCAL << 16,    /* source memory type */
        MAILBOX_WORD0,              /* destination low */
        CCP_MEMTYPE_LOCAL << 16,    /* destination memory type */
    };

    if (!qts) {
        return;
    }
    qtest_writel(qts, MAILBOX_INT_ENABLE, MAILBOX_INT_ENABLE_SECOS);
    qtest_writel(qts, SRAM_SRC, COMMAND);
    for (int i = 0; i < ARRAY_SIZE(desc); i++) {
        qtest_writel(qts, SRAM_DESC + 4 * i, desc[i]);
    }
    qtest_writel(qts, CCP_Q0 + CCP_Q_TAIL, SRAM_DESC);
    qtest_writel(qts, CCP_Q0 + CCP_Q_HEAD, SRAM_DESC + CCP_DESC_SIZE);
    qtest_writel(qts, CCP_Q0 + CCP_Q_CTRL, CCP_Q_RUN);
    /* The queue runs when the control register is read with RUN set */
    qtest_readl(qts, CCP_Q0 + CCP_Q_CTRL);
    g_assert_cmpuint(qtest_readl(qts, CCP_Q0 + CCP_Q_STATUS), ==, 0);
    g_assert_cmphex(qtest_readl(qts, MAILBOX_WORD0), ==, COMMAND);
    ic_drain(qts, pending);
    assert_only(pending, SRC_CCP);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_autoptr(GError) err = NULL;
    static const uint8_t flash[4096];
    int fd, ret;

    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(MACHINE)) {
        skip_reason = "machine " MACHINE " not available";
    } else {
        /* The machine needs a flash image; an empty one is enough here */
        fd = g_file_open_tmp("psp-flash-XXXXXX", &flash_path, &err);
        if (fd < 0 || write(fd, flash, sizeof(flash)) != sizeof(flash)) {
            skip_reason = "cannot create a flash image";
        }
        if (fd >= 0) {
            close(fd);
        }
    }

    qtest_add_func("/amd-psp/mailbox/external-write-armed",
                   test_external_write_armed);
    qtest_add_func("/amd-psp/mailbox/external-write-unarmed",
                   test_external_write_unarmed);
    qtest_add_func("/amd-psp/mailbox/ccp-copy", test_ccp_copy);

    ret = g_test_run();
    if (flash_path) {
        unlink(flash_path);
        g_free(flash_path);
    }
    return ret;
}
