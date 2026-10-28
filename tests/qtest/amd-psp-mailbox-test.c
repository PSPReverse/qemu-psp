/*
 * QTest for the AMD PSP mailbox doorbell: only writes from outside the guest
 * raise it, and a CCP copy onto the mailbox is not one of them. Also the
 * word layout and the sub-word access rules of the mailbox itself.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "amd-psp-test.h"

#define COMMAND 0x00010000

/* Arm the doorbell as the firmware does, write word 0, return the sources */
static void ring_and_drain(QTestState *qts, uint32_t base, uint32_t enable,
                           bool pending[IC_SOURCES])
{
    qtest_writel(qts, MBOX_INT_ENABLE, enable);
    qtest_writel(qts, MBOX_WORD(0), COMMAND);
    psp_ic_drain(qts, base, pending);
}

/* An external write to word 0 rings the doorbell once it is armed */
static void test_external_write_armed(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    bool pending[IC_SOURCES];

    if (!qts) {
        return;
    }
    ring_and_drain(qts, IC_BASE, MBOX_INT_ENABLE_SECOS, pending);
    psp_ic_assert_only(pending, SRC_DOORBELL);
    qtest_quit(qts);
}

/* Without the enable register it stays silent */
static void test_external_write_unarmed(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    bool pending[IC_SOURCES];

    if (!qts) {
        return;
    }
    ring_and_drain(qts, IC_BASE, 0, pending);
    psp_ic_assert_only(pending, -1);
    qtest_quit(qts);
}

/* Only the SecOS bits of the enable register arm it; any one of them does */
static void test_enable_bits(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    bool pending[IC_SOURCES];

    if (!qts) {
        return;
    }
    ring_and_drain(qts, IC_BASE, ~MBOX_INT_ENABLE_SECOS, pending);
    psp_ic_assert_only(pending, -1);
    ring_and_drain(qts, IC_BASE, 0x01000000, pending);
    psp_ic_assert_only(pending, SRC_DOORBELL);
    ring_and_drain(qts, IC_BASE, 0x80000000, pending);
    psp_ic_assert_only(pending, SRC_DOORBELL);
    qtest_quit(qts);
}

/* The enable register reads back what was written, all 32 bits */
static void test_enable_readback(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    g_assert_cmphex(qtest_readl(qts, MBOX_INT_ENABLE), ==, 0);
    qtest_writel(qts, MBOX_INT_ENABLE, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(qts, MBOX_INT_ENABLE), ==, 0xdeadbeef);
    qtest_quit(qts);
}

/* Words 1 and 2 hold data; writing them never rings, armed or not */
static void test_other_words_do_not_ring(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    bool pending[IC_SOURCES];

    if (!qts) {
        return;
    }
    qtest_writel(qts, MBOX_INT_ENABLE, MBOX_INT_ENABLE_SECOS);
    qtest_writel(qts, MBOX_WORD(1), 0x11223344);
    qtest_writel(qts, MBOX_WORD(2), 0x55667788);
    qtest_writeb(qts, MBOX_WORD(1) + 3, 0xaa);
    qtest_writew(qts, MBOX_WORD(2), 0xbbcc);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, -1);
    g_assert_cmphex(qtest_readl(qts, MBOX_WORD(1)), ==, 0xaa223344);
    g_assert_cmphex(qtest_readl(qts, MBOX_WORD(2)), ==, 0x5566bbcc);
    g_assert_cmphex(qtest_readl(qts, MBOX_WORD(0)), ==, 0);
    qtest_quit(qts);
}

/* Sub-word accesses touch only their bytes and read back by lane */
static void test_subword_access(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    qtest_writel(qts, MBOX_WORD(0), 0xaabbccdd);
    qtest_writew(qts, MBOX_WORD(0) + 2, 0x1122);
    g_assert_cmphex(qtest_readl(qts, MBOX_WORD(0)), ==, 0x1122ccdd);
    qtest_writeb(qts, MBOX_WORD(0) + 1, 0xee);
    g_assert_cmphex(qtest_readl(qts, MBOX_WORD(0)), ==, 0x1122eedd);
    g_assert_cmphex(qtest_readb(qts, MBOX_WORD(0) + 0), ==, 0xdd);
    g_assert_cmphex(qtest_readb(qts, MBOX_WORD(0) + 1), ==, 0xee);
    g_assert_cmphex(qtest_readb(qts, MBOX_WORD(0) + 3), ==, 0x11);
    g_assert_cmphex(qtest_readw(qts, MBOX_WORD(0) + 0), ==, 0xeedd);
    g_assert_cmphex(qtest_readw(qts, MBOX_WORD(0) + 2), ==, 0x1122);
    qtest_quit(qts);
}

/*
 * A partial external write that touches word 0 rings like a full one: the
 * doorbell keys on the word, not on the access width.
 */
static void test_subword_write_rings(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    bool pending[IC_SOURCES];

    if (!qts) {
        return;
    }
    qtest_writel(qts, MBOX_INT_ENABLE, MBOX_INT_ENABLE_SECOS);
    qtest_writeb(qts, MBOX_WORD(0) + 3, 1);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, SRC_DOORBELL);

    qtest_writew(qts, MBOX_WORD(0), 0x1234);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, SRC_DOORBELL);
    qtest_quit(qts);
}

/* Writes ring edge-wise: after an acknowledge, the next write rings again */
static void test_ring_ack_ring(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    bool pending[IC_SOURCES];

    if (!qts) {
        return;
    }
    qtest_writel(qts, MBOX_INT_ENABLE, MBOX_INT_ENABLE_SECOS);
    /* Two writes before the acknowledge are one pending source */
    qtest_writel(qts, MBOX_WORD(0), COMMAND);
    qtest_writel(qts, MBOX_WORD(0), COMMAND + 1);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, SRC_DOORBELL);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, -1);
    qtest_writel(qts, MBOX_WORD(0), COMMAND + 2);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, SRC_DOORBELL);
    qtest_quit(qts);
}

/* The doorbell is Zen2-only by default; the property moves or disables it */
static void test_doorbell_property(void)
{
    QTestState *qts;
    bool pending[IC_SOURCES];

    qts = psp_start(PSP_ZEN3);
    if (!qts) {
        return;
    }
    ring_and_drain(qts, IC_BASE, MBOX_INT_ENABLE_SECOS, pending);
    psp_ic_assert_only(pending, -1);
    qtest_quit(qts);

    qts = psp_startf(PSP_ZEN3, "-global driver=amd_psp.mailbox,"
                     "property=doorbell_irq_id,value=%d", SRC_DOORBELL);
    ring_and_drain(qts, IC_BASE, MBOX_INT_ENABLE_SECOS, pending);
    psp_ic_assert_only(pending, SRC_DOORBELL);
    qtest_quit(qts);

    qts = psp_startf(PSP_ZEN2, "-global driver=amd_psp.mailbox,"
                     "property=doorbell_irq_id,value=-1");
    ring_and_drain(qts, IC_BASE, MBOX_INT_ENABLE_SECOS, pending);
    psp_ic_assert_only(pending, -1);
    qtest_quit(qts);

    qts = psp_startf(PSP_ZEN2, "-global driver=amd_psp.mailbox,"
                     "property=doorbell_irq_id,value=7");
    ring_and_drain(qts, IC_BASE, MBOX_INT_ENABLE_SECOS, pending);
    psp_ic_assert_only(pending, 7);
    qtest_quit(qts);
}

/*
 * The write-only "write" property rings the doorbell over QMP like an
 * external write, and rejects malformed input.
 */
static void test_qom_write(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    bool pending[IC_SOURCES];
    QDict *rsp;

    if (!qts) {
        return;
    }
    qtest_writel(qts, MBOX_INT_ENABLE, MBOX_INT_ENABLE_SECOS);
    qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments': "
                             "{'path': %s, 'property': 'write', "
                             "'value': '0,0x1b0000'}}", MBOX_PATH);
    g_assert_cmphex(qtest_readl(qts, MBOX_WORD(0)), ==, 0x1b0000);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, SRC_DOORBELL);

    qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments': "
                             "{'path': %s, 'property': 'write', "
                             "'value': '2,42'}}", MBOX_PATH);
    g_assert_cmpuint(qtest_readl(qts, MBOX_WORD(2)), ==, 42);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, -1);

    const char *bad[] = { "3,1", "0", "x,1", "0,y", "0,0x100000000", "" };
    for (int i = 0; i < ARRAY_SIZE(bad); i++) {
        rsp = qtest_qmp(qts, "{'execute': 'qom-set', 'arguments': "
                        "{'path': %s, 'property': 'write', "
                        "'value': %s}}", MBOX_PATH, bad[i]);
        g_assert_true(qdict_haskey(rsp, "error"));
        qobject_unref(rsp);
    }
    qtest_quit(qts);
}

/*
 * A CCP passthrough copy onto word 0 lands there, but it is CCP DMA, not an
 * external write: only the CCP completion is raised.
 */
static void test_ccp_copy(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    bool pending[IC_SOURCES];

    if (!qts) {
        return;
    }
    qtest_writel(qts, MBOX_INT_ENABLE, MBOX_INT_ENABLE_SECOS);
    qtest_writel(qts, SRAM_SRC, COMMAND);
    g_assert_cmpuint(psp_ccp_copy(qts, MBOX_WORD(0), SRAM_SRC, 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, MBOX_WORD(0)), ==, COMMAND);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, SRC_CCP);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);

    qtest_add_func("/amd-psp/mailbox/external-write-armed",
                   test_external_write_armed);
    qtest_add_func("/amd-psp/mailbox/external-write-unarmed",
                   test_external_write_unarmed);
    qtest_add_func("/amd-psp/mailbox/enable-bits", test_enable_bits);
    qtest_add_func("/amd-psp/mailbox/enable-readback", test_enable_readback);
    qtest_add_func("/amd-psp/mailbox/other-words-do-not-ring",
                   test_other_words_do_not_ring);
    qtest_add_func("/amd-psp/mailbox/subword-access", test_subword_access);
    qtest_add_func("/amd-psp/mailbox/subword-write-rings",
                   test_subword_write_rings);
    qtest_add_func("/amd-psp/mailbox/ring-ack-ring", test_ring_ack_ring);
    qtest_add_func("/amd-psp/mailbox/doorbell-property",
                   test_doorbell_property);
    qtest_add_func("/amd-psp/mailbox/qom-write", test_qom_write);
    qtest_add_func("/amd-psp/mailbox/ccp-copy", test_ccp_copy);

    return psp_test_run();
}
