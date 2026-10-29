/*
 * QTest for the Zen3 SEV firmware mailbox: read/write-back words, the
 * doorbell on IC source 0x40 for writes from outside the guest only, the
 * "write" and "words" properties, and its absence on Zen2.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "amd-psp-test.h"

#define COMMAND 0x00040000

static void sev_write(QTestState *qts, const char *value)
{
    qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments': "
                             "{'path': %s, 'property': 'write', "
                             "'value': %s}}", SEV_PATH, value);
}

static char *sev_words(QTestState *qts)
{
    QDict *rsp = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                           "{'path': %s, 'property': 'words'}}", SEV_PATH);
    char *v;

    g_assert_true(qdict_haskey(rsp, "return"));
    v = g_strdup(qdict_get_str(rsp, "return"));
    qobject_unref(rsp);
    return v;
}

/* The three words read back whole and by lane; only word 0 rings */
static void test_readback(void)
{
    QTestState *qts = psp_start(PSP_ZEN3);
    bool pending[IC_SOURCES];

    if (!qts) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        g_assert_cmphex(qtest_readl(qts, SEV_WORD(i)), ==, 0);
    }
    qtest_writel(qts, SEV_WORD(1), 0x11223344);
    qtest_writel(qts, SEV_WORD(2), 0x55667788);
    qtest_writeb(qts, SEV_WORD(1) + 3, 0xaa);
    qtest_writew(qts, SEV_WORD(2), 0xbbcc);
    g_assert_cmphex(qtest_readl(qts, SEV_WORD(1)), ==, 0xaa223344);
    g_assert_cmphex(qtest_readl(qts, SEV_WORD(2)), ==, 0x5566bbcc);
    g_assert_cmphex(qtest_readw(qts, SEV_WORD(2) + 2), ==, 0x5566);
    g_assert_cmphex(qtest_readl(qts, SEV_WORD(0)), ==, 0);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, -1);
    qtest_quit(qts);
}

/* An external command write rings source 0x40 once; so does a re-ring */
static void test_external_write_rings(void)
{
    QTestState *qts = psp_start(PSP_ZEN3);
    bool pending[IC_SOURCES];

    if (!qts) {
        return;
    }
    qtest_writel(qts, SEV_WORD(0), COMMAND);
    g_assert_cmphex(qtest_readl(qts, SEV_WORD(0)), ==, COMMAND);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, SRC_SEV);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, -1);
    qtest_writel(qts, SEV_WORD(0), COMMAND + 1);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, SRC_SEV);
    qtest_quit(qts);
}

/* A reply (bit 31 set) written from outside is not a command */
static void test_reply_does_not_ring(void)
{
    QTestState *qts = psp_start(PSP_ZEN3);
    bool pending[IC_SOURCES];

    if (!qts) {
        return;
    }
    qtest_writel(qts, SEV_WORD(0), 0x80000000 | COMMAND);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, -1);
    qtest_quit(qts);
}

/* A CCP copy carries explicit attributes: it lands, but does not ring */
static void test_ccp_copy_does_not_ring(void)
{
    QTestState *qts = psp_start(PSP_ZEN3);
    bool pending[IC_SOURCES];

    if (!qts) {
        return;
    }
    qtest_writel(qts, SRAM_SRC, COMMAND);
    g_assert_cmpuint(psp_ccp_copy(qts, SEV_WORD(0), SRAM_SRC, 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SEV_WORD(0)), ==, COMMAND);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, SRC_CCP);
    qtest_quit(qts);
}

/* "write" rings like an external write and rejects bad input */
static void test_qom_write_words(void)
{
    QTestState *qts = psp_start(PSP_ZEN3);
    bool pending[IC_SOURCES];
    QDict *rsp;
    char *w;

    if (!qts) {
        return;
    }
    w = sev_words(qts);
    g_assert_cmpstr(w, ==, "0x0,0x0,0x0");
    g_free(w);

    sev_write(qts, "1,0x1000000");
    sev_write(qts, "2,0");
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, -1);
    sev_write(qts, "0,0x40000");
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, SRC_SEV);
    /* The BL's reply goes through the bus, not the property */
    qtest_writel(qts, SEV_WORD(0), 0x80040000);
    w = sev_words(qts);
    g_assert_cmpstr(w, ==, "0x80040000,0x1000000,0x0");
    g_free(w);

    const char *bad[] = { "3,1", "0", "x,1", "0,y", "0,0x100000000", "" };
    for (int i = 0; i < ARRAY_SIZE(bad); i++) {
        rsp = qtest_qmp(qts, "{'execute': 'qom-set', 'arguments': "
                        "{'path': %s, 'property': 'write', "
                        "'value': %s}}", SEV_PATH, bad[i]);
        g_assert_true(qdict_haskey(rsp, "error"));
        qobject_unref(rsp);
    }
    rsp = qtest_qmp(qts, "{'execute': 'qom-set', 'arguments': "
                    "{'path': %s, 'property': 'words', 'value': 'x'}}",
                    SEV_PATH);
    g_assert_true(qdict_haskey(rsp, "error"));
    qobject_unref(rsp);
    qtest_quit(qts);
}

/* The property moves the source or disables it */
static void test_doorbell_property(void)
{
    QTestState *qts;
    bool pending[IC_SOURCES];

    qts = psp_startf(PSP_ZEN3, "-global driver=amd_psp.sev-mailbox,"
                     "property=doorbell_irq_id,value=-1");
    qtest_writel(qts, SEV_WORD(0), COMMAND);
    g_assert_cmphex(qtest_readl(qts, SEV_WORD(0)), ==, COMMAND);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, -1);
    qtest_quit(qts);

    qts = psp_startf(PSP_ZEN3, "-global driver=amd_psp.sev-mailbox,"
                     "property=doorbell_irq_id,value=7");
    qtest_writel(qts, SEV_WORD(0), COMMAND);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, 7);
    qtest_quit(qts);
}

/* Zen2 has no SEV mailbox: no device, and the addresses are not ours */
static void test_zen2_absent(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    bool pending[IC_SOURCES];
    QDict *rsp;

    if (!qts) {
        return;
    }
    rsp = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                    "{'path': %s, 'property': 'words'}}", SEV_PATH);
    g_assert_true(qdict_haskey(rsp, "error"));
    qobject_unref(rsp);
    qtest_writel(qts, SEV_WORD(0), COMMAND);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, -1);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);

    qtest_add_func("/amd-psp/sev-mailbox/readback", test_readback);
    qtest_add_func("/amd-psp/sev-mailbox/external-write-rings",
                   test_external_write_rings);
    qtest_add_func("/amd-psp/sev-mailbox/reply-does-not-ring",
                   test_reply_does_not_ring);
    qtest_add_func("/amd-psp/sev-mailbox/ccp-copy-does-not-ring",
                   test_ccp_copy_does_not_ring);
    qtest_add_func("/amd-psp/sev-mailbox/qom-write-words",
                   test_qom_write_words);
    qtest_add_func("/amd-psp/sev-mailbox/doorbell-property",
                   test_doorbell_property);
    qtest_add_func("/amd-psp/sev-mailbox/zen2-absent", test_zen2_absent);

    return psp_test_run();
}
