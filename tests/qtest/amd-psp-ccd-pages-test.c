/*
 * QTest for the Zen3 per-CCD pages the SEV applet reads in SNP_INIT: RAM
 * windows with initialised words (SMN 0x2400000 | ccd << 20) and constant
 * status words in the neighbouring units. Values from psp-etl 13528.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "amd-psp-test.h"

#define SMN_CTRL 0x03220000
#define SMN_WINDOW0 0x01000000          /* PSP address of slot 0 */

static void select_smn(QTestState *qts, unsigned smn_slot)
{
    qtest_writew(qts, SMN_CTRL, smn_slot);
}

/* Initial words, and the page keeps what is written to it */
static void test_ccd_page(void)
{
    QTestState *qts = psp_start(PSP_ZEN3);

    if (!qts) {
        return;
    }
    for (unsigned ccd = 0; ccd < 4; ccd++) {
        select_smn(qts, 0x24 + ccd);
        g_assert_cmphex(qtest_readl(qts, SMN_WINDOW0 + 0x18), ==, 1);
        g_assert_cmphex(qtest_readl(qts, SMN_WINDOW0 + 0x34), ==, 0x80000000);
        g_assert_cmphex(qtest_readl(qts, SMN_WINDOW0 + 0x1c), ==, 0);
        qtest_writel(qts, SMN_WINDOW0 + 0x100, 0xfffff000 + ccd);
    }
    for (unsigned ccd = 0; ccd < 4; ccd++) {
        select_smn(qts, 0x24 + ccd);
        g_assert_cmphex(qtest_readl(qts, SMN_WINDOW0 + 0x100), ==,
                        0xfffff000 + ccd);
    }
    qtest_quit(qts);
}

/* Unit status words, one per unit and ccd, only the checked word is set */
static void test_unit_status(void)
{
    static const unsigned units[] = { 0x153, 0x14f, 0x147, 0x14b };
    QTestState *qts = psp_start(PSP_ZEN3);

    if (!qts) {
        return;
    }
    for (unsigned ccd = 0; ccd < 4; ccd++) {
        for (int i = 0; i < 4; i++) {
            select_smn(qts, units[i] + ccd);
            g_assert_cmphex(qtest_readl(qts, SMN_WINDOW0 + 0x4018), ==, 1);
            g_assert_cmphex(qtest_readl(qts, SMN_WINDOW0 + 0xdc), ==, 0);
        }
        select_smn(qts, 0x157 + ccd);
        g_assert_cmphex(qtest_readl(qts, SMN_WINDOW0 + 0x4018), ==, 1);
        g_assert_cmphex(qtest_readl(qts, SMN_WINDOW0 + 0x18), ==, 0);
    }
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);

    qtest_add_func("/amd-psp/ccd-pages/page", test_ccd_page);
    qtest_add_func("/amd-psp/ccd-pages/unit-status", test_unit_status);
    return psp_test_run();
}
