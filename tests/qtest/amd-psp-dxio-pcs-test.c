/*
 * QTest for the Zen3 DXIO PCS state registers. Register layout comes from
 * the emulator source, not from firmware.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "amd-psp-test.h"

/* SMN control (include/psp/smn/psp-smn.h): 16 bit slot n at +2n */
#define SMN_CTRL 0x03220000
#define SMN_WINDOW0 0x01000000          /* PSP address of slot 0, 1 MiB */
#define PCS_FIRST 0x12e
#define PCS_COUNT 6
#define PCS_CMD_ADDR (SMN_WINDOW0 + 0xf0040)
#define PCS_HIST (SMN_WINDOW0 + 0xf0078)

static void select_pcs(QTestState *qts, unsigned idx)
{
    qtest_writew(qts, SMN_CTRL, idx);
}

static void test_command(void)
{
    QTestState *qts = psp_start(PSP_ZEN3);

    if (!qts) {
        return;
    }
    select_pcs(qts, PCS_FIRST);
    g_assert_cmphex(qtest_readl(qts, PCS_HIST) & 0xff, ==, 0);
    qtest_writel(qts, PCS_CMD_ADDR, 0x42);
    g_assert_cmphex(qtest_readl(qts, PCS_CMD_ADDR), ==, 0);
    g_assert_cmphex(qtest_readl(qts, PCS_HIST) & 0xff, ==, 0x42);
    g_assert_cmphex(qtest_readb(qts, PCS_HIST), ==, 0x42);
    qtest_writel(qts, PCS_CMD_ADDR, 0x11);
    g_assert_cmphex(qtest_readl(qts, PCS_CMD_ADDR), ==, 0);
    g_assert_cmphex(qtest_readl(qts, PCS_HIST) & 0xff, ==, 0x11);
    qtest_quit(qts);
}

static void test_isolation(void)
{
    QTestState *qts = psp_start(PSP_ZEN3);

    if (!qts) {
        return;
    }
    for (unsigned i = 0; i < PCS_COUNT; i++) {
        select_pcs(qts, PCS_FIRST + i);
        qtest_writel(qts, PCS_CMD_ADDR, 0x40 + i);
    }
    for (unsigned i = 0; i < PCS_COUNT; i++) {
        select_pcs(qts, PCS_FIRST + i);
        g_assert_cmphex(qtest_readl(qts, PCS_HIST) & 0xff, ==, 0x40 + i);
        g_assert_cmphex(qtest_readl(qts, PCS_CMD_ADDR), ==, 0);
    }
    /* Neighbouring instances stay plain unimplemented space */
    select_pcs(qts, PCS_FIRST - 1);
    qtest_writel(qts, PCS_CMD_ADDR, 0x42);
    g_assert_cmphex(qtest_readl(qts, PCS_HIST), ==, 0);
    select_pcs(qts, PCS_FIRST + PCS_COUNT);
    qtest_writel(qts, PCS_CMD_ADDR, 0x42);
    g_assert_cmphex(qtest_readl(qts, PCS_HIST), ==, 0);
    qtest_quit(qts);
}

static void test_other_registers(void)
{
    QTestState *qts = psp_start(PSP_ZEN3);

    if (!qts) {
        return;
    }
    select_pcs(qts, PCS_FIRST);
    qtest_writel(qts, SMN_WINDOW0 + 0xf0000, 0x1234);
    g_assert_cmphex(qtest_readl(qts, SMN_WINDOW0 + 0xf0000), ==, 0);
    qtest_quit(qts);
}

static void test_ready_bits(void)
{
    QTestState *qts = psp_start(PSP_ZEN3);

    if (!qts) {
        return;
    }
    for (unsigned idx = PCS_FIRST; idx < PCS_FIRST + 4; idx++) {
        select_pcs(qts, idx);
        for (unsigned k = 0; k < 4; k++) {
            g_assert_cmphex(qtest_readl(qts, SMN_WINDOW0 + 0xe04fc +
                                        k * 0x1000), ==, 0xa00);
        }
    }
    qtest_quit(qts);
}

static void test_smn_1d75c(void)
{
    QTestState *qts = psp_start(PSP_ZEN3);

    if (!qts) {
        return;
    }
    select_pcs(qts, 0);
    g_assert_cmphex(qtest_readl(qts, SMN_WINDOW0 + 0x1d75c) & 0x7f, ==, 0);
    qtest_quit(qts);
}

static void test_zen2_absent(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    select_pcs(qts, PCS_FIRST);
    qtest_writel(qts, PCS_CMD_ADDR, 0x42);
    g_assert_cmphex(qtest_readl(qts, PCS_HIST), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SMN_WINDOW0 + 0xe04fc), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);

    qtest_add_func("/amd-psp/dxio-pcs/command", test_command);
    qtest_add_func("/amd-psp/dxio-pcs/isolation", test_isolation);
    qtest_add_func("/amd-psp/dxio-pcs/other-registers", test_other_registers);
    qtest_add_func("/amd-psp/dxio-pcs/ready-bits", test_ready_bits);
    qtest_add_func("/amd-psp/dxio-pcs/smn-1d75c", test_smn_1d75c);
    qtest_add_func("/amd-psp/dxio-pcs/zen2-absent", test_zen2_absent);

    return psp_test_run();
}
