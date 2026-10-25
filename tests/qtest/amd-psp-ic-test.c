/*
 * QTest for the AMD PSP interrupt controller: enable registers, pending
 * source reporting, acknowledge. Sources are raised through the controller's
 * "inject" QOM property, the same GPIO input the devices use.
 *
 * The enable mask applying to FIQ_NEXT and FIQ_ALL_PROCESSED is a pending
 * fix, see amd-psp-pending-fixes-test.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "amd-psp-test.h"

/* The four enable words are independent and read back */
static void test_enable_readback(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    for (int g = 0; g < 4; g++) {
        g_assert_cmphex(qtest_readl(qts, IC_ENABLE(IC_BASE, g)), ==, 0);
    }
    qtest_writel(qts, IC_ENABLE(IC_BASE, 1), 0x12345678);
    qtest_writel(qts, IC_ENABLE(IC_BASE, 3), 0x80000001);
    g_assert_cmphex(qtest_readl(qts, IC_ENABLE(IC_BASE, 0)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, IC_ENABLE(IC_BASE, 1)), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, IC_ENABLE(IC_BASE, 2)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, IC_ENABLE(IC_BASE, 3)), ==, 0x80000001);
    qtest_quit(qts);
}

/* Nothing pending: all processed, next source 0, unknown offsets read 0 */
static void test_idle(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    psp_ic_enable_all(qts, IC_BASE);
    g_assert_cmpuint(qtest_readl(qts, IC_FIQ_ALL_PROCESSED(IC_BASE)), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, IC_FIQ_NEXT(IC_BASE)), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, IC_BASE + 0x20), ==, 0);
    qtest_quit(qts);
}

/* FIQ_NEXT reports the lowest pending source, across the four groups */
static void test_lowest_first(void)
{
    static const unsigned order[] = {0x05, 0x20, 0x41, 0x7f};
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    psp_ic_enable_all(qts, IC_BASE);
    /* Raised out of order */
    psp_ic_inject(qts, 0x41);
    psp_ic_inject(qts, 0x05);
    psp_ic_inject(qts, 0x7f);
    psp_ic_inject(qts, 0x20);
    for (int i = 0; i < ARRAY_SIZE(order); i++) {
        g_assert_cmpuint(qtest_readl(qts, IC_FIQ_ALL_PROCESSED(IC_BASE)),
                         ==, 0);
        g_assert_cmpuint(qtest_readl(qts, IC_FIQ_NEXT(IC_BASE)), ==, order[i]);
        /* Reading does not acknowledge */
        g_assert_cmpuint(qtest_readl(qts, IC_FIQ_NEXT(IC_BASE)), ==, order[i]);
        qtest_writel(qts, IC_FIQ_HANDLED(IC_BASE, order[i] / 32),
                     1u << (order[i] % 32));
    }
    g_assert_cmpuint(qtest_readl(qts, IC_FIQ_ALL_PROCESSED(IC_BASE)), ==, 1);
    qtest_quit(qts);
}

/* An acknowledge clears only the bits it names, in the group it names */
static void test_ack_scope(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    psp_ic_enable_all(qts, IC_BASE);
    psp_ic_inject(qts, 0x41);               /* group 2, bit 1 */
    /* Same bit, other group; other bit, same group */
    qtest_writel(qts, IC_FIQ_HANDLED(IC_BASE, 0), 1u << 1);
    qtest_writel(qts, IC_FIQ_HANDLED(IC_BASE, 1), 1u << 1);
    qtest_writel(qts, IC_FIQ_HANDLED(IC_BASE, 2), ~(1u << 1));
    qtest_writel(qts, IC_FIQ_HANDLED(IC_BASE, 2), 0);
    g_assert_cmpuint(qtest_readl(qts, IC_FIQ_ALL_PROCESSED(IC_BASE)), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, IC_FIQ_NEXT(IC_BASE)), ==, 0x41);
    qtest_writel(qts, IC_FIQ_HANDLED(IC_BASE, 2), 1u << 1);
    g_assert_cmpuint(qtest_readl(qts, IC_FIQ_ALL_PROCESSED(IC_BASE)), ==, 1);
    qtest_quit(qts);
}

/* A source is a bit: raising it twice before the acknowledge is one event */
static void test_raise_is_idempotent(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    bool pending[IC_SOURCES];

    if (!qts) {
        return;
    }
    psp_ic_inject(qts, 0x33);
    psp_ic_inject(qts, 0x33);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, 0x33);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, -1);
    qtest_quit(qts);
}

/*
 * Edge behaviour: a source stays acknowledged until its device raises the
 * line again; raising after an acknowledge makes it pending again.
 */
static void test_reraise_after_ack(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    bool pending[IC_SOURCES];

    if (!qts) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        psp_ic_inject(qts, 0x10);
        psp_ic_drain(qts, IC_BASE, pending);
        psp_ic_assert_only(pending, 0x10);
    }
    qtest_quit(qts);
}

/* The hook rejects a source the controller does not have */
static void test_inject_range(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    bool pending[IC_SOURCES];
    QDict *rsp;

    if (!qts) {
        return;
    }
    rsp = qtest_qmp(qts, "{'execute': 'qom-set', 'arguments': "
                    "{'path': %s, 'property': 'inject', 'value': %d}}",
                    IC_PATH, IC_SOURCES);
    g_assert_true(qdict_haskey(rsp, "error"));
    qobject_unref(rsp);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, -1);

    psp_ic_inject(qts, IC_SOURCES - 1);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, IC_SOURCES - 1);
    qtest_quit(qts);
}

/* Zen and Zen+ have the same block at 0x03010300 */
static void test_zen_layout(void)
{
    QTestState *qts = psp_start(PSP_ZEN);
    bool pending[IC_SOURCES];

    if (!qts) {
        return;
    }
    qtest_writel(qts, IC_ENABLE(IC_BASE_ZEN, 2), 0xa5a5a5a5);
    g_assert_cmphex(qtest_readl(qts, IC_ENABLE(IC_BASE_ZEN, 2)), ==,
                    0xa5a5a5a5);
    psp_ic_inject(qts, 0x40);
    psp_ic_inject(qts, 0x63);
    psp_ic_drain(qts, IC_BASE_ZEN, pending);
    for (int i = 0; i < IC_SOURCES; i++) {
        g_assert_cmpint(pending[i], ==, i == 0x40 || i == 0x63);
    }
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);

    qtest_add_func("/amd-psp/ic/enable-readback", test_enable_readback);
    qtest_add_func("/amd-psp/ic/idle", test_idle);
    qtest_add_func("/amd-psp/ic/lowest-first", test_lowest_first);
    qtest_add_func("/amd-psp/ic/ack-scope", test_ack_scope);
    qtest_add_func("/amd-psp/ic/raise-is-idempotent", test_raise_is_idempotent);
    qtest_add_func("/amd-psp/ic/reraise-after-ack", test_reraise_after_ack);
    qtest_add_func("/amd-psp/ic/inject-range", test_inject_range);
    qtest_add_func("/amd-psp/ic/zen-layout", test_zen_layout);

    return psp_test_run();
}
