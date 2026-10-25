/*
 * QTest for the vmstate of the AMD PSP devices: a machine with non-default
 * device state is saved with the "migrate" QMP command to a file and loaded
 * into a second machine with "-incoming defer" / "migrate-incoming". The
 * second machine starts from power-on state, so equality afterwards shows
 * the state came through the stream, not from reset.
 *
 * What cannot be covered: the SHA and zlib contexts are deliberately not
 * migrated (a snapshot taken mid-hash restores with no context), and the
 * "stop"/"savevm" route needs a block device the machines do not have, so
 * only the migration stream is exercised.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "amd-psp-test.h"

static void test_mailbox(void)
{
    QTestState *src = psp_start(PSP_ZEN2), *dst;
    bool pending[IC_SOURCES];

    if (!src) {
        return;
    }
    qtest_writel(src, MBOX_WORD(0), 0x01234567);
    qtest_writel(src, MBOX_WORD(1), 0x89abcdef);
    qtest_writel(src, MBOX_WORD(2), 0xfedcba98);
    qtest_writel(src, MBOX_INT_ENABLE, MBOX_INT_ENABLE_SECOS);
    dst = psp_migrate_clone(src, PSP_ZEN2, "");
    qtest_quit(src);

    g_assert_cmphex(qtest_readl(dst, MBOX_WORD(0)), ==, 0x01234567);
    g_assert_cmphex(qtest_readl(dst, MBOX_WORD(1)), ==, 0x89abcdef);
    g_assert_cmphex(qtest_readl(dst, MBOX_WORD(2)), ==, 0xfedcba98);
    g_assert_cmphex(qtest_readl(dst, MBOX_INT_ENABLE), ==,
                    MBOX_INT_ENABLE_SECOS);
    /* The restored enable register arms the doorbell */
    qtest_writel(dst, MBOX_WORD(0), 1);
    psp_ic_drain(dst, IC_BASE, pending);
    psp_ic_assert_only(pending, SRC_DOORBELL);
    qtest_quit(dst);
}

static void test_ic(void)
{
    QTestState *src = psp_start(PSP_ZEN2), *dst;
    bool pending[IC_SOURCES];

    if (!src) {
        return;
    }
    qtest_writel(src, IC_ENABLE(IC_BASE, 0), 0x0000ffff);
    qtest_writel(src, IC_ENABLE(IC_BASE, 1), 0xa5a5a5a5);
    qtest_writel(src, IC_ENABLE(IC_BASE, 2), 0x5a5a5a5a);
    qtest_writel(src, IC_ENABLE(IC_BASE, 3), 0xffff0000);
    psp_ic_inject(src, 0x05);
    psp_ic_inject(src, 0x41);
    psp_ic_inject(src, 0x7e);
    dst = psp_migrate_clone(src, PSP_ZEN2, "");
    qtest_quit(src);

    g_assert_cmphex(qtest_readl(dst, IC_ENABLE(IC_BASE, 0)), ==, 0x0000ffff);
    g_assert_cmphex(qtest_readl(dst, IC_ENABLE(IC_BASE, 1)), ==, 0xa5a5a5a5);
    g_assert_cmphex(qtest_readl(dst, IC_ENABLE(IC_BASE, 2)), ==, 0x5a5a5a5a);
    g_assert_cmphex(qtest_readl(dst, IC_ENABLE(IC_BASE, 3)), ==, 0xffff0000);
    psp_ic_drain(dst, IC_BASE, pending);
    for (int i = 0; i < IC_SOURCES; i++) {
        g_assert_cmpint(pending[i], ==, i == 0x05 || i == 0x41 || i == 0x7e);
    }
    qtest_quit(dst);
}

static void test_timer(void)
{
    QTestState *src = psp_start(PSP_ZEN2), *dst;

    if (!src) {
        return;
    }
    for (int t = 0; t < 2; t++) {
        uint32_t base = t ? TIMER2 : TIMER1;

        qtest_writel(src, base + TIMER_MISC(0), 0x10 + t);
        qtest_writel(src, base + TIMER_MISC(2), 0x30 + t);
        qtest_writel(src, base + TIMER_PERIOD, 0x1000 + t);
        qtest_writel(src, base + TIMER_PLACEHOLDER(1), 0x50 + t);
        qtest_writel(src, base + TIMER_COUNT, 0x70 + t);
        qtest_writel(src, base + TIMER_CONTROL, 0x100 + t);
    }
    dst = psp_migrate_clone(src, PSP_ZEN2, "");
    qtest_quit(src);

    for (int t = 0; t < 2; t++) {
        uint32_t base = t ? TIMER2 : TIMER1;

        g_assert_cmphex(qtest_readl(dst, base + TIMER_MISC(0)), ==, 0x10 + t);
        g_assert_cmphex(qtest_readl(dst, base + TIMER_MISC(1)), ==, 0);
        g_assert_cmphex(qtest_readl(dst, base + TIMER_MISC(2)), ==, 0x30 + t);
        g_assert_cmphex(qtest_readl(dst, base + TIMER_PERIOD), ==, 0x1000 + t);
        g_assert_cmphex(qtest_readl(dst, base + TIMER_PLACEHOLDER(1)), ==,
                        0x50 + t);
        g_assert_cmphex(qtest_readl(dst, base + TIMER_CONTROL), ==, 0x100 + t);
        /* Control bit 0 is set only on timer 2, so only it counts per read */
        g_assert_cmphex(qtest_readl(dst, base + TIMER_COUNT), ==, 0x70 + t);
    }
    qtest_quit(dst);
}

/* Queue registers, the interrupt latch, the LSB, the zlib counter, the TRNG */
static void test_ccp(void)
{
    uint8_t want[32], got[32];
    uint32_t ref[3];
    QTestState *src, *dst, *refm;

    src = psp_start(PSP_ZEN2);
    if (!src) {
        return;
    }
    qtest_writel(src, CCP_Q0 + 0x1000 + CCP_Q_HEAD, 0x4321);
    qtest_writel(src, CCP_Q0 + 0x1000 + CCP_Q_TAIL, 0x1234);

    /* A SHA digest in LSB slot 3, a zlib counter, one run on queue 0 */
    psp_expected_sha256("abcd", 4, want);
    qtest_memwrite(src, SRAM_SRC, "abcd", 4);
    g_assert_cmpuint(psp_ccp_sha256(src, SRAM_SRC, 4, 3,
                                    CCP_INIT | CCP_EOM), ==, 0);
    qtest_memwrite(src, SRAM_SRC, psp_zlib_abcd, sizeof(psp_zlib_abcd));
    g_assert_cmpuint(psp_ccp_zlib(src, sizeof(psp_zlib_abcd)), ==, 0);
    /* Two random words drawn: the stream must continue from the third */
    qtest_readl(src, CCP_TRNG);
    qtest_readl(src, CCP_TRNG);

    dst = psp_migrate_clone(src, PSP_ZEN2, "");
    qtest_quit(src);

    g_assert_cmphex(qtest_readl(dst, CCP_Q0 + 0x1000 + CCP_Q_HEAD), ==,
                    0x4321);
    g_assert_cmphex(qtest_readl(dst, CCP_Q0 + 0x1000 + CCP_Q_TAIL), ==,
                    0x1234);
    g_assert_cmphex(qtest_readl(dst, CCP_Q0 + CCP_Q_CTRL), ==, CCP_Q_HALT);
    g_assert_cmphex(qtest_readl(dst, CCP_Q0 + CCP_Q_HEAD), ==,
                    qtest_readl(dst, CCP_Q0 + CCP_Q_TAIL));
    g_assert_cmphex(qtest_readl(dst, CCP_INT_STATUS), ==,
                    CCP_INT_STATUS_DONE);
    g_assert_cmpuint(qtest_readl(dst, CCP_ZLIB_WRITTEN), ==, 4);
    psp_ccp_read_digest(dst, 3, got);
    g_assert_cmpmem(got, 32, want, 32);

    /* The reference machine never migrated: it shows the third word */
    refm = psp_start(PSP_ZEN2);
    for (int i = 0; i < 3; i++) {
        ref[i] = qtest_readl(refm, CCP_TRNG);
    }
    qtest_quit(refm);
    g_assert_cmphex(qtest_readl(dst, CCP_TRNG), ==, ref[2]);
    qtest_quit(dst);
}

/* The x86 slot mapper restores its registers and re-points the slots */
static void test_x86_mapper(void)
{
    QTestState *src = psp_start(PSP_ZEN2), *dst;

    if (!src) {
        return;
    }
    qtest_writel(src, 0x03230000 + 16 + 4, 0x13579bdf);
    qtest_writel(src, 0x03230000 + 32 + 12, 0x2468ace0);
    dst = psp_migrate_clone(src, PSP_ZEN2, "");
    qtest_quit(src);
    g_assert_cmphex(qtest_readl(dst, 0x03230000 + 16 + 4), ==, 0x13579bdf);
    g_assert_cmphex(qtest_readl(dst, 0x03230000 + 32 + 12), ==, 0x2468ace0);
    qtest_quit(dst);
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);

    qtest_add_func("/amd-psp/migrate/mailbox", test_mailbox);
    qtest_add_func("/amd-psp/migrate/ic", test_ic);
    qtest_add_func("/amd-psp/migrate/timer", test_timer);
    qtest_add_func("/amd-psp/migrate/ccp", test_ccp);
    qtest_add_func("/amd-psp/migrate/x86-mapper", test_x86_mapper);

    return psp_test_run();
}
