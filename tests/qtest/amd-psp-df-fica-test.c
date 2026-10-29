/*
 * QTest for the Zen3 data fabric indirect configuration access pair. The
 * FICAA layout comes from the emulator source, not from firmware.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "amd-psp-test.h"

/* SMN control (include/psp/smn/psp-smn.h): 16 bit slot n at +2n */
#define SMN_CTRL 0x03220000
#define SMN_WINDOW0 0x01000000          /* PSP address of slot 0 */
#define FICAA (SMN_WINDOW0 + 0x1d050)
#define FICAD (SMN_WINDOW0 + 0x1d080)

/* function [13:11], register [10:2], instance [23:16] */
#define KEY(inst, func, reg) (((inst) << 16) | ((func) << 11) | (reg))

static QTestState *start(const char *gen)
{
    QTestState *qts = psp_start(gen);

    if (qts) {
        qtest_writew(qts, SMN_CTRL, 0);
    }
    return qts;
}

static void df_write(QTestState *qts, uint32_t key, uint32_t val)
{
    qtest_writel(qts, FICAA, key);
    qtest_writel(qts, FICAD, val);
}

static uint32_t df_read(QTestState *qts, uint32_t key)
{
    qtest_writel(qts, FICAA, key);
    return qtest_readl(qts, FICAD);
}

static void test_independent(void)
{
    QTestState *qts = start(PSP_ZEN3);

    if (!qts) {
        return;
    }
    df_write(qts, KEY(0x1e, 0, 0x110), 0x11111111);
    df_write(qts, KEY(0x1e, 0, 0x114), 0x22222222);
    df_write(qts, KEY(0x1e, 1, 0x110), 0x33333333);
    g_assert_cmphex(df_read(qts, KEY(0x1e, 0, 0x110)), ==, 0x11111111);
    g_assert_cmphex(df_read(qts, KEY(0x1e, 0, 0x114)), ==, 0x22222222);
    g_assert_cmphex(df_read(qts, KEY(0x1e, 1, 0x110)), ==, 0x33333333);
    /* Overwrite keeps the other registers */
    df_write(qts, KEY(0x1e, 0, 0x110), 0x44444444);
    g_assert_cmphex(df_read(qts, KEY(0x1e, 0, 0x110)), ==, 0x44444444);
    g_assert_cmphex(df_read(qts, KEY(0x1e, 0, 0x114)), ==, 0x22222222);
    g_assert_cmphex(qtest_readl(qts, FICAA), ==, KEY(0x1e, 0, 0x114));
    qtest_quit(qts);
}

static void test_instance_ignored(void)
{
    QTestState *qts = start(PSP_ZEN3);

    if (!qts) {
        return;
    }
    df_write(qts, KEY(0x1e, 0, 0x110), 0xcafe0001);
    g_assert_cmphex(df_read(qts, KEY(0x00, 0, 0x110)), ==, 0xcafe0001);
    g_assert_cmphex(df_read(qts, KEY(0xff, 0, 0x110)), ==, 0xcafe0001);
    /* The access type bit is not part of the lookup either */
    g_assert_cmphex(df_read(qts, KEY(0x07, 0, 0x110) | 1), ==, 0xcafe0001);
    qtest_quit(qts);
}

static void test_unwritten_reads_zero(void)
{
    QTestState *qts = start(PSP_ZEN3);

    if (!qts) {
        return;
    }
    g_assert_cmphex(df_read(qts, KEY(0, 0, 0x100)), ==, 0);
    g_assert_cmphex(df_read(qts, KEY(0x1e, 2, 0x7fc)), ==, 0);
    qtest_quit(qts);
}

static void test_hole_base_default(void)
{
    QTestState *qts = start(PSP_ZEN3);

    if (!qts) {
        return;
    }
    /* Served until written, for any instance, only for function 0 */
    g_assert_cmphex(df_read(qts, KEY(0, 0, 0x104)), ==, 0xb0000001);
    g_assert_cmphex(df_read(qts, KEY(0x1e, 0, 0x104)), ==, 0xb0000001);
    g_assert_cmphex(df_read(qts, KEY(0, 1, 0x104)), ==, 0);
    df_write(qts, KEY(0, 0, 0x104), 0x80000001);
    g_assert_cmphex(df_read(qts, KEY(0, 0, 0x104)), ==, 0x80000001);
    /* Writing zero is a write, not the default */
    df_write(qts, KEY(0, 0, 0x104), 0);
    g_assert_cmphex(df_read(qts, KEY(0, 0, 0x104)), ==, 0);
    qtest_quit(qts);
}

static void test_hole_base_default_off(void)
{
    QTestState *qts = psp_startf(PSP_ZEN3, "-global "
                                 "driver=amd_psp.smn.df-fica,"
                                 "property=hole-base-default,value=0");

    if (!qts) {
        return;
    }
    qtest_writew(qts, SMN_CTRL, 0);
    g_assert_cmphex(df_read(qts, KEY(0, 0, 0x104)), ==, 0);
    qtest_quit(qts);
}

static void test_zen2_absent(void)
{
    QTestState *qts = start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    df_write(qts, KEY(0, 0, 0x110), 0x1234);
    g_assert_cmphex(df_read(qts, KEY(0, 0, 0x110)), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);

    qtest_add_func("/amd-psp/df-fica/independent", test_independent);
    qtest_add_func("/amd-psp/df-fica/instance-ignored",
                   test_instance_ignored);
    qtest_add_func("/amd-psp/df-fica/unwritten-reads-zero",
                   test_unwritten_reads_zero);
    qtest_add_func("/amd-psp/df-fica/hole-base-default",
                   test_hole_base_default);
    qtest_add_func("/amd-psp/df-fica/hole-base-default-off",
                   test_hole_base_default_off);
    qtest_add_func("/amd-psp/df-fica/zen2-absent", test_zen2_absent);

    return psp_test_run();
}
