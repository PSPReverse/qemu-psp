/*
 * QTest for the Zen3 MP5 stand-in. Register layout comes from the emulator
 * source, not from firmware.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "amd-psp-test.h"

/* SMN control (include/psp/smn/psp-smn.h): 16 bit slot n at +2n */
#define SMN_CTRL 0x03220000
#define SMN_WINDOW(n) (0x01000000 + (n) * 0x100000)
#define MP5_CTRL (SMN_WINDOW(0) + 0x8c)      /* SMN 0x3030008c */
#define MP5_FLAG (SMN_WINDOW(0) + 0xe0)      /* SMN 0x303000e0 */
#define MP5_WORD (SMN_WINDOW(1) + 0x808)        /* SMN 0x30000808 */
#define MP5_NODE_STATUS (SMN_WINDOW(2) + 0x10024) /* SMN 0x30410024 */
#define MP5_MAILBOX 0x0301071c
#define MP5_COMMAND 0x001c0000
#define SRC_MP5 0x10

static QTestState *start(const char *machine, const char *extra)
{
    QTestState *qts = psp_startf(machine, "%s", extra);

    if (qts) {
        qtest_writew(qts, SMN_CTRL + 0, 0x303);
        qtest_writew(qts, SMN_CTRL + 2, 0x300);
        qtest_writew(qts, SMN_CTRL + 4, 0x304);
        psp_ic_enable_all(qts, IC_BASE);
    }
    return qts;
}

/* The off-chip BL's sequence, MP5 firmware load left out */
static void start_mp5(QTestState *qts)
{
    qtest_writel(qts, MP5_CTRL, 2);
    qtest_writel(qts, MP5_FLAG, 2);
    qtest_writel(qts, MP5_CTRL, 0);
}

static void test_answer(void)
{
    QTestState *qts = start(PSP_ZEN3, "");

    if (!qts) {
        return;
    }
    g_assert_cmphex(qtest_readl(qts, IC_FIQ_ALL_PROCESSED(IC_BASE)), ==, 1);
    start_mp5(qts);
    /* Nothing before the delay (1000 us) */
    qtest_clock_step(qts, 999000);
    g_assert_cmphex(qtest_readl(qts, MP5_MAILBOX), ==, 0);
    g_assert_cmphex(qtest_readl(qts, IC_FIQ_ALL_PROCESSED(IC_BASE)), ==, 1);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readl(qts, MP5_MAILBOX), ==, MP5_COMMAND);
    g_assert_cmphex(qtest_readl(qts, IC_FIQ_ALL_PROCESSED(IC_BASE)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, IC_FIQ_NEXT(IC_BASE)), ==, SRC_MP5);
    /* Stays pending until acknowledged */
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(qtest_readl(qts, IC_FIQ_NEXT(IC_BASE)), ==, SRC_MP5);
    qtest_writel(qts, IC_FIQ_HANDLED(IC_BASE, 0), 1u << SRC_MP5);
    g_assert_cmphex(qtest_readl(qts, IC_FIQ_ALL_PROCESSED(IC_BASE)), ==, 1);
    /* The BL's answer lands */
    qtest_writel(qts, MP5_MAILBOX, MP5_COMMAND | 0x80000000);
    g_assert_cmphex(qtest_readl(qts, MP5_MAILBOX), ==,
                    MP5_COMMAND | 0x80000000);
    qtest_quit(qts);
}

static void test_delay_property(void)
{
    QTestState *qts = start(PSP_ZEN3, "-global driver=amd_psp.smn.mp5,"
                            "property=delay-us,value=50");

    if (!qts) {
        return;
    }
    start_mp5(qts);
    qtest_clock_step(qts, 49000);
    g_assert_cmphex(qtest_readl(qts, MP5_MAILBOX), ==, 0);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readl(qts, MP5_MAILBOX), ==, MP5_COMMAND);
    qtest_quit(qts);
}

/* A write of 0 without the preceding 2 does not start the MP5 */
static void test_needs_load_write(void)
{
    QTestState *qts = start(PSP_ZEN3, "");

    if (!qts) {
        return;
    }
    qtest_writel(qts, MP5_CTRL, 0);
    qtest_clock_step(qts, 10000000);
    g_assert_cmphex(qtest_readl(qts, MP5_MAILBOX), ==, 0);
    g_assert_cmphex(qtest_readl(qts, IC_FIQ_ALL_PROCESSED(IC_BASE)), ==, 1);
    qtest_quit(qts);
}

static void test_disabled(void)
{
    QTestState *qts = start(PSP_ZEN3, "-global driver=amd_psp.smn.mp5,"
                            "property=enabled,value=off");

    if (!qts) {
        return;
    }
    start_mp5(qts);
    qtest_clock_step(qts, 10000000);
    g_assert_cmphex(qtest_readl(qts, MP5_MAILBOX), ==, 0);
    g_assert_cmphex(qtest_readl(qts, IC_FIQ_ALL_PROCESSED(IC_BASE)), ==, 1);
    qtest_quit(qts);
}

static void test_registers(void)
{
    QTestState *qts = start(PSP_ZEN3, "");

    if (!qts) {
        return;
    }
    g_assert_cmphex(qtest_readl(qts, MP5_WORD), ==, 0x00100000);
    g_assert_cmphex(qtest_readl(qts, MP5_WORD + 4), ==, 0);
    qtest_writel(qts, MP5_WORD, 0);
    g_assert_cmphex(qtest_readl(qts, MP5_WORD), ==, 0);
    g_assert_cmphex(qtest_readl(qts, MP5_NODE_STATUS), ==, 1);
    qtest_writel(qts, MP5_FLAG, 2);
    g_assert_cmphex(qtest_readl(qts, MP5_FLAG), ==, 2);
    qtest_writel(qts, MP5_CTRL, 2);
    g_assert_cmphex(qtest_readl(qts, MP5_CTRL), ==, 2);
    qtest_quit(qts);
}

/* The stand-in does not exist on Zen2 */
static void test_zen2_absent(void)
{
    QTestState *qts = start(PSP_ZEN2, "");

    if (!qts) {
        return;
    }
    start_mp5(qts);
    qtest_clock_step(qts, 10000000);
    g_assert_cmphex(qtest_readl(qts, MP5_MAILBOX), ==, 0);
    g_assert_cmphex(qtest_readl(qts, MP5_CTRL), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);

    qtest_add_func("/amd-psp/mp5/answer", test_answer);
    qtest_add_func("/amd-psp/mp5/delay-property", test_delay_property);
    qtest_add_func("/amd-psp/mp5/needs-load-write", test_needs_load_write);
    qtest_add_func("/amd-psp/mp5/disabled", test_disabled);
    qtest_add_func("/amd-psp/mp5/registers", test_registers);
    qtest_add_func("/amd-psp/mp5/zen2-absent", test_zen2_absent);

    return psp_test_run();
}
