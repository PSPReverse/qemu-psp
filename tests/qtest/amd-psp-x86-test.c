/*
 * QTest for the AMD PSP x86 side: the slot mapper registers and the
 * placeholder I/O port 0x61 reached through a mapped slot. Reset and
 * migration of port 0x61 are pending fixes, see
 * amd-psp-pending-fixes-test.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "amd-psp-test.h"

/* The mapper registers are plain per-slot storage */
static void test_mapper_registers(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    for (int slot = 0; slot < 3; slot++) {
        for (int reg = 0; reg < 4; reg++) {
            g_assert_cmphex(qtest_readl(qts, X86_CTRL + slot * X86_SLOT_REGS +
                                        4 * reg), ==, 0);
        }
    }
    qtest_writel(qts, X86_CTRL + 1 * X86_SLOT_REGS + 4, 0xabcd0123);
    qtest_writel(qts, X86_CTRL + 2 * X86_SLOT_REGS + 8, 0x01020304);
    g_assert_cmphex(qtest_readl(qts, X86_CTRL + 1 * X86_SLOT_REGS + 4), ==,
                    0xabcd0123);
    g_assert_cmphex(qtest_readl(qts, X86_CTRL + 2 * X86_SLOT_REGS + 8), ==,
                    0x01020304);
    g_assert_cmphex(qtest_readl(qts, X86_CTRL + 1 * X86_SLOT_REGS + 8), ==, 0);
    /* Sub-word accesses use their byte lane */
    qtest_writeb(qts, X86_CTRL + 1 * X86_SLOT_REGS + 4 + 1, 0xee);
    g_assert_cmphex(qtest_readl(qts, X86_CTRL + 1 * X86_SLOT_REGS + 4), ==,
                    0xabcdee23);
    qtest_quit(qts);
}

/*
 * Port 0x61: bits 0-1 store what was written; bit 4 toggles on every read
 * (a stand-in for the refresh clock). Inferred behaviour, see the comment in
 * psp/x86/psp-x86-misc.c.
 */
static void test_port61(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    uint8_t a, b, c;

    if (!qts) {
        return;
    }
    qtest_writel(qts, X86_CTRL, X86_BASE_PROTECTED_MMIO);

    a = qtest_readb(qts, PORT61);
    b = qtest_readb(qts, PORT61);
    c = qtest_readb(qts, PORT61);
    g_assert_cmphex(a & 0x10, !=, b & 0x10);
    g_assert_cmphex(a & 0x10, ==, c & 0x10);
    g_assert_cmphex(a & 0x2f, ==, 0);

    qtest_writeb(qts, PORT61, 0xff);
    a = qtest_readb(qts, PORT61);
    b = qtest_readb(qts, PORT61);
    g_assert_cmphex(a & 0xef, ==, 0x03);
    g_assert_cmphex(b & 0xef, ==, 0x03);
    g_assert_cmphex(a & 0x10, !=, b & 0x10);
    qtest_writeb(qts, PORT61, 0x01);
    g_assert_cmphex(qtest_readb(qts, PORT61) & 0xef, ==, 0x01);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);

    qtest_add_func("/amd-psp/x86/mapper-registers", test_mapper_registers);
    qtest_add_func("/amd-psp/x86/port61", test_port61);

    return psp_test_run();
}
