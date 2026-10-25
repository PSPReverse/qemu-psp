/*
 * QTest for system_reset on the AMD PSP machines: devices return to their
 * power-on state. The state the reset fixes in the pending group
 * (CCP queue registers, port 0x61) is in amd-psp-pending-fixes-test.c.
 *
 * The CPU itself cannot be told apart before and after a reset here: the
 * qtest accelerator runs no code and no test can write a register, so the
 * PC check below only guards the boot vector.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "amd-psp-test.h"

static void assert_pc_at_boot_vector(QTestState *qts)
{
    g_autofree char *regs = qtest_hmp(qts, "info registers");

    g_assert_nonnull(strstr(regs, "R15=ffff0000"));
}

static void test_mailbox(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    bool pending[IC_SOURCES];

    if (!qts) {
        return;
    }
    qtest_writel(qts, MBOX_WORD(0), 0x11111111);
    qtest_writel(qts, MBOX_WORD(1), 0x22222222);
    qtest_writel(qts, MBOX_WORD(2), 0x33333333);
    qtest_writel(qts, MBOX_INT_ENABLE, MBOX_INT_ENABLE_SECOS);
    psp_system_reset(qts);
    for (int i = 0; i < 3; i++) {
        g_assert_cmphex(qtest_readl(qts, MBOX_WORD(i)), ==, 0);
    }
    g_assert_cmphex(qtest_readl(qts, MBOX_INT_ENABLE), ==, 0);
    /* Disarmed again, and the interrupt it raised is gone */
    qtest_writel(qts, MBOX_WORD(0), 1);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, -1);
    qtest_quit(qts);
}

static void test_ic(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    psp_ic_enable_all(qts, IC_BASE);
    psp_ic_inject(qts, 0x05);
    psp_ic_inject(qts, 0x6f);
    g_assert_cmpuint(qtest_readl(qts, IC_FIQ_ALL_PROCESSED(IC_BASE)), ==, 0);
    psp_system_reset(qts);
    for (int g = 0; g < 4; g++) {
        g_assert_cmphex(qtest_readl(qts, IC_ENABLE(IC_BASE, g)), ==, 0);
    }
    g_assert_cmpuint(qtest_readl(qts, IC_FIQ_ALL_PROCESSED(IC_BASE)), ==, 1);
    qtest_quit(qts);
}

static void test_timer(void)
{
    QTestState *qts = psp_startf(PSP_ZEN2,
                                 "-global driver=amd_psp.timer,"
                                 "property=expiry_irq_id,value=17");
    bool pending[IC_SOURCES];

    if (!qts) {
        return;
    }
    for (int t = 0; t < 2; t++) {
        uint32_t base = t ? TIMER2 : TIMER1;

        qtest_writel(qts, base + TIMER_PERIOD, 100);
        qtest_writel(qts, base + TIMER_MISC(1), 5);
        qtest_writel(qts, base + TIMER_PLACEHOLDER(2), 6);
        qtest_writel(qts, base + TIMER_COUNT, 77);
        qtest_writel(qts, base + TIMER_CONTROL, 1);
    }
    psp_system_reset(qts);
    for (int t = 0; t < 2; t++) {
        uint32_t base = t ? TIMER2 : TIMER1;

        g_assert_cmphex(qtest_readl(qts, base + TIMER_CONTROL), ==, 0);
        g_assert_cmphex(qtest_readl(qts, base + TIMER_PERIOD), ==, 0);
        g_assert_cmphex(qtest_readl(qts, base + TIMER_MISC(1)), ==, 0);
        g_assert_cmphex(qtest_readl(qts, base + TIMER_PLACEHOLDER(2)), ==, 0);
        g_assert_cmphex(qtest_readl(qts, base + TIMER_COUNT), ==, 0);
    }
    /* The armed expiry timer was cancelled */
    qtest_clock_step(qts, 1000000);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, -1);
    qtest_quit(qts);
}

/* The random sequence starts over and the zlib byte counter clears */
static void test_ccp(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    uint32_t first, second;

    if (!qts) {
        return;
    }
    first = qtest_readl(qts, CCP_TRNG);
    second = qtest_readl(qts, CCP_TRNG);
    g_assert_cmphex(first, !=, second);
    qtest_memwrite(qts, SRAM_SRC, psp_zlib_abcd, sizeof(psp_zlib_abcd));
    g_assert_cmpuint(psp_ccp_zlib(qts, sizeof(psp_zlib_abcd)), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, CCP_ZLIB_WRITTEN), ==, 4);

    psp_system_reset(qts);
    g_assert_cmpuint(qtest_readl(qts, CCP_ZLIB_WRITTEN), ==, 0);
    /* A narrow read shows the last word drawn: none yet */
    g_assert_cmphex(qtest_readb(qts, CCP_TRNG), ==, 0);
    g_assert_cmphex(qtest_readl(qts, CCP_TRNG), ==, first);
    g_assert_cmphex(qtest_readl(qts, CCP_TRNG), ==, second);
    qtest_quit(qts);
}

static void test_cpu_boot_vector(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    assert_pc_at_boot_vector(qts);
    psp_system_reset(qts);
    assert_pc_at_boot_vector(qts);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);

    qtest_add_func("/amd-psp/reset/mailbox", test_mailbox);
    qtest_add_func("/amd-psp/reset/ic", test_ic);
    qtest_add_func("/amd-psp/reset/timer", test_timer);
    qtest_add_func("/amd-psp/reset/ccp", test_ccp);
    qtest_add_func("/amd-psp/reset/cpu-boot-vector", test_cpu_boot_vector);

    return psp_test_run();
}
