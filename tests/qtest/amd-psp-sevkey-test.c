/*
 * QTest for the Zen3 SEV key-slot ports in the CCX SMN windows
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "amd-psp-test.h"

#define SMN_CTRL 0x03220000
#define SMN_WINDOW0 0x01000000          /* PSP address of slot 0 */
/* CCX n: SMN 0x50000 | n << 20, the slot register holds n */
#define CCX(n) (SMN_WINDOW0 + 0x50000)
#define INDEX(n) (CCX(n) + 0xa00)
#define DATA(n) (CCX(n) + 0xa04)
#define SCRATCH(n) (CCX(n) + 0x11ac)

static void select_ccx(QTestState *qts, unsigned n)
{
    qtest_writew(qts, SMN_CTRL, n);
}

/* Write four words through the port, rewind, read them back per slot */
static void test_key_readback(void)
{
    static const uint32_t key_a[4] = { 0x11111111, 0x22222222, 0x33333333,
                                       0x44444444 };
    static const uint32_t key_b[4] = { 0xaaaaaaaa, 0xbbbbbbbb, 0xcccccccc,
                                       0xdddddddd };
    QTestState *qts = psp_start(PSP_ZEN3);

    if (!qts) {
        return;
    }
    select_ccx(qts, 0);
    qtest_writel(qts, INDEX(0), 0x80000000 | 2 << 2);
    for (int i = 0; i < 4; i++) {
        qtest_writel(qts, DATA(0), key_a[i]);
    }
    qtest_writel(qts, INDEX(0), 0x80000000 | 3 << 2);
    for (int i = 0; i < 4; i++) {
        qtest_writel(qts, DATA(0), key_b[i]);
    }
    qtest_writel(qts, INDEX(0), 0x80000000 | 2 << 2);
    for (int i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readl(qts, DATA(0)), ==, key_a[i]);
    }
    qtest_writel(qts, INDEX(0), 0x80000000 | 3 << 2);
    for (int i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readl(qts, DATA(0)), ==, key_b[i]);
    }
    /* Rewinding is what restarts at word 0 */
    qtest_writel(qts, INDEX(0), 0x80000000 | 2 << 2);
    g_assert_cmphex(qtest_readl(qts, DATA(0)), ==, key_a[0]);
    qtest_quit(qts);
}

static void test_scratch(void)
{
    QTestState *qts = psp_start(PSP_ZEN3);

    if (!qts) {
        return;
    }
    select_ccx(qts, 0);
    qtest_writel(qts, SCRATCH(0), 0x20020000);
    g_assert_cmphex(qtest_readl(qts, SCRATCH(0)), ==, 0x20020000);
    qtest_quit(qts);
}

/* Zen2 has no key ports: the index register is not storage */
static void test_zen2_absent(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    select_ccx(qts, 0);
    qtest_writel(qts, DATA(0), 0x1234);
    g_assert_cmphex(qtest_readl(qts, DATA(0)), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);
    qtest_add_func("/amd-psp/sevkey/key-readback", test_key_readback);
    qtest_add_func("/amd-psp/sevkey/scratch", test_scratch);
    qtest_add_func("/amd-psp/sevkey/zen2-absent", test_zen2_absent);
    return psp_test_run();
}
