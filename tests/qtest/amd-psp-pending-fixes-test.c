/*
 * AMD PSP qtests for behaviour that is NOT in the integration pin yet. Each
 * test names the branch that carries its fix. They are registered but
 * skipped unless PSP_QTEST_PENDING=1, so the suite stays green on the pin;
 * run them with the variable set on a build that includes the branch, and
 * move a test to its device's file when the branch is merged.
 *
 * Not expressible with libqtest, and so not here:
 *  - claude/psp-reset-fixes, CPU reset: the qtest accelerator runs no code
 *    and no register can be written, so a reset CPU looks like an untouched
 *    one. Needs a TCG test.
 *  - claude/psp-timer-fixes, a counter write consuming aspfuzz_timer_advance
 *    offset: the function is a host export, not reachable from qtest.
 *  - claude/psp-doorbell-debug and claude/psp-mailbox-write: a debug write
 *    (virtual-address, MemTxAttrs.debug) and aspfuzz_mailbox_write; see
 *    test_doorbell_debug_write.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "amd-psp-test.h"

#define EXPIRY_ARGS "-global driver=amd_psp.timer,property=expiry_irq_id," \
                    "value=17"

static bool pending_enabled(void)
{
    const char *e = g_getenv("PSP_QTEST_PENDING");

    return e && !strcmp(e, "1");
}

/* Skip (and return true) unless pending tests are enabled and runnable */
static bool pending_skip(const char *branch)
{
    if (!pending_enabled()) {
        g_autofree char *why =
            g_strdup_printf("pending fix on %s, set PSP_QTEST_PENDING=1",
                            branch);

        g_test_skip(why);
        return true;
    }
    if (psp_skip_reason) {
        g_test_skip(psp_skip_reason);
        return true;
    }
    return false;
}

/* Start with the unimp log in a file; the file is read after quit */
static QTestState *start_logged(char **path)
{
    int fd = g_file_open_tmp("psp-log-XXXXXX", path, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    return psp_startf(PSP_ZEN2, "-d unimp -D %s", *path);
}

static char *read_log(char *path)
{
    char *log = NULL;

    g_assert_true(g_file_get_contents(path, &log, NULL, NULL));
    unlink(path);
    g_free(path);
    return log;
}

static void map_protected_mmio(QTestState *qts)
{
    qtest_writel(qts, X86_CTRL, X86_BASE_PROTECTED_MMIO);
}

/* claude/psp-ccp-fixes: every LSB slot owns its SHA context */
static void test_sha256_context_per_slot(void)
{
    QTestState *qts;
    uint8_t want0[32], want1[32], got[32];

    if (pending_skip("claude/psp-ccp-fixes")) {
        return;
    }
    qts = psp_start(PSP_ZEN2);
    psp_expected_sha256("abcd", 4, want0);
    psp_expected_sha256("xyz!", 4, want1);
    qtest_memwrite(qts, SRAM_SRC, "abcdxyz!", 8);
    /* Interleave two hashes: slot 0 "ab" "cd", slot 1 "xy" "z!" */
    g_assert_cmpuint(psp_ccp_sha256(qts, SRAM_SRC + 0, 2, 0, CCP_INIT), ==, 0);
    g_assert_cmpuint(psp_ccp_sha256(qts, SRAM_SRC + 4, 2, 1, CCP_INIT), ==, 0);
    g_assert_cmpuint(psp_ccp_sha256(qts, SRAM_SRC + 2, 2, 0, CCP_EOM), ==, 0);
    g_assert_cmpuint(psp_ccp_sha256(qts, SRAM_SRC + 6, 2, 1, CCP_EOM), ==, 0);
    psp_ccp_read_digest(qts, 0, got);
    g_assert_cmpmem(got, 32, want0, 32);
    psp_ccp_read_digest(qts, 1, got);
    g_assert_cmpmem(got, 32, want1, 32);
    qtest_quit(qts);
}

/*
 * claude/psp-ccp-fixes: a continuation without a live context hashes only
 * its own data and says so with a PSP_TODO line.
 */
static void test_sha256_continuation_without_context(void)
{
    char *path = NULL;
    QTestState *qts;
    g_autofree char *log = NULL;
    uint8_t want[32], got[32];

    if (pending_skip("claude/psp-ccp-fixes")) {
        return;
    }
    qts = start_logged(&path);
    psp_expected_sha256("tail", 4, want);
    qtest_memwrite(qts, SRAM_SRC, "tail", 4);
    g_assert_cmpuint(psp_ccp_sha256(qts, SRAM_SRC, 4, 5, CCP_EOM), ==, 0);
    psp_ccp_read_digest(qts, 5, got);
    g_assert_cmpmem(got, 32, want, 32);
    qtest_quit(qts);
    log = read_log(path);
    g_assert_nonnull(strstr(log, "PSP_TODO ccp.sha256.continuation_no_context"));
}

/* claude/psp-ccp-fixes: a failed inflate is a PSP_TODO, not a silent gap */
static void test_zlib_error_is_reported(void)
{
    char *path = NULL;
    QTestState *qts;
    g_autofree char *log = NULL;
    uint8_t garbage[16];
    PspCcpDesc d = {
        .engine = CCP_ENGINE_ZLIB, .flags = CCP_INIT | CCP_EOM,
        .len = sizeof(garbage),
        .src = SRAM_SRC, .src_mem = CCP_MEMTYPE_LOCAL,
        .dst = SRAM_DST, .dst_mem = CCP_MEMTYPE_LOCAL,
    };

    if (pending_skip("claude/psp-ccp-fixes")) {
        return;
    }
    qts = start_logged(&path);
    memset(garbage, 0xff, sizeof(garbage));
    qtest_memwrite(qts, SRAM_SRC, garbage, sizeof(garbage));
    psp_ccp_run(qts, &d);
    g_assert_cmpuint(qtest_readl(qts, CCP_ZLIB_WRITTEN), ==, 0);
    qtest_quit(qts);
    log = read_log(path);
    g_assert_nonnull(strstr(log, "PSP_TODO ccp.zlib.inflate"));
}

/* claude/psp-ccp-fixes: reset returns every queue, the status and the latch */
static void test_ccp_reset(void)
{
    QTestState *qts;

    if (pending_skip("claude/psp-ccp-fixes")) {
        return;
    }
    qts = psp_start(PSP_ZEN2);
    qtest_memwrite(qts, SRAM_SRC, "abcd", 4);
    g_assert_cmpuint(psp_ccp_copy(qts, SRAM_DST, SRAM_SRC, 4), ==, 0);
    qtest_writel(qts, CCP_Q0 + 0x2000 + CCP_Q_TAIL, 0x20);
    qtest_writel(qts, CCP_Q0 + 0x4000 + CCP_Q_HEAD, 0x40);
    qtest_writel(qts, CCP_Q0 + 0x3000 + CCP_Q_STATUS, 1);
    g_assert_cmphex(qtest_readl(qts, CCP_INT_STATUS), ==,
                    CCP_INT_STATUS_DONE);
    psp_system_reset(qts);
    for (int q = 0; q < 5; q++) {
        uint32_t base = CCP_Q0 + 0x1000 * q;

        g_assert_cmphex(qtest_readl(qts, base + CCP_Q_CTRL), ==, CCP_Q_HALT);
        g_assert_cmphex(qtest_readl(qts, base + CCP_Q_HEAD), ==, 0);
        g_assert_cmphex(qtest_readl(qts, base + CCP_Q_TAIL), ==, 0);
        g_assert_cmphex(qtest_readl(qts, base + CCP_Q_STATUS), ==, 0);
    }
    g_assert_cmphex(qtest_readl(qts, CCP_INT_STATUS), ==, 0);
    qtest_quit(qts);
}

/*
 * claude/psp-timer-fixes: the expiry phase survives a save and load: a
 * timer 4 us into a 10 us period fires 6 us after the restore, not 10.
 */
static void test_timer_expiry_phase_across_restore(void)
{
    QTestState *src, *dst;
    bool pending[IC_SOURCES];

    if (pending_skip("claude/psp-timer-fixes")) {
        return;
    }
    src = psp_startf(PSP_ZEN2, EXPIRY_ARGS);
    qtest_writel(src, TIMER1 + TIMER_PERIOD, 1000);     /* 10 us */
    qtest_writel(src, TIMER1 + TIMER_CONTROL, 1);
    qtest_clock_step(src, 4000);
    dst = psp_migrate_clone(src, PSP_ZEN2, EXPIRY_ARGS);
    qtest_quit(src);

    qtest_clock_step(dst, 5999);
    psp_ic_drain(dst, IC_BASE, pending);
    psp_ic_assert_only(pending, -1);
    qtest_clock_step(dst, 1);
    psp_ic_drain(dst, IC_BASE, pending);
    psp_ic_assert_only(pending, 17);
    qtest_quit(dst);
}

/*
 * claude/psp-ic-mask: FIQ_NEXT and FIQ_ALL_PROCESSED only report sources the
 * firmware has enabled, as the core line does.
 */
static void test_ic_enable_mask(void)
{
    QTestState *qts;

    if (pending_skip("claude/psp-ic-mask")) {
        return;
    }
    qts = psp_start(PSP_ZEN2);
    psp_ic_inject(qts, 0x05);
    psp_ic_inject(qts, 0x41);
    /* Source 5 stays masked, 0x41 is enabled */
    qtest_writel(qts, IC_ENABLE(IC_BASE, 0), ~(1u << 5));
    qtest_writel(qts, IC_ENABLE(IC_BASE, 2), UINT32_MAX);
    g_assert_cmpuint(qtest_readl(qts, IC_FIQ_ALL_PROCESSED(IC_BASE)), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, IC_FIQ_NEXT(IC_BASE)), ==, 0x41);
    qtest_writel(qts, IC_FIQ_HANDLED(IC_BASE, 2), 1u << 1);
    g_assert_cmpuint(qtest_readl(qts, IC_FIQ_ALL_PROCESSED(IC_BASE)), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, IC_FIQ_NEXT(IC_BASE)), ==, 0);
    /* Enabling it later makes the still-pending source visible */
    qtest_writel(qts, IC_ENABLE(IC_BASE, 0), UINT32_MAX);
    g_assert_cmpuint(qtest_readl(qts, IC_FIQ_ALL_PROCESSED(IC_BASE)), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, IC_FIQ_NEXT(IC_BASE)), ==, 5);
    qtest_quit(qts);
}

/* claude/psp-reset-fixes: system_reset clears the port 0x61 state */
static void test_port61_reset(void)
{
    QTestState *qts;

    if (pending_skip("claude/psp-reset-fixes")) {
        return;
    }
    qts = psp_start(PSP_ZEN2);
    map_protected_mmio(qts);
    qtest_writeb(qts, PORT61, 0x03);
    qtest_readb(qts, PORT61);                   /* leaves the toggle set */
    psp_system_reset(qts);
    map_protected_mmio(qts);
    g_assert_cmphex(qtest_readb(qts, PORT61), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, PORT61), ==, 0x10);
    qtest_quit(qts);
}

/* claude/psp-reset-fixes: port 0x61 is part of the migrated state */
static void test_port61_migrate(void)
{
    QTestState *src, *dst;

    if (pending_skip("claude/psp-reset-fixes")) {
        return;
    }
    src = psp_start(PSP_ZEN2);
    map_protected_mmio(src);
    qtest_writeb(src, PORT61, 0x02);
    g_assert_cmphex(qtest_readb(src, PORT61), ==, 0x02);    /* toggle is set */
    dst = psp_migrate_clone(src, PSP_ZEN2, "");
    qtest_quit(src);
    g_assert_cmphex(qtest_readb(dst, PORT61), ==, 0x12);
    g_assert_cmphex(qtest_readb(dst, PORT61), ==, 0x02);
    qtest_quit(dst);
}

/*
 * claude/psp-doorbell-debug and claude/psp-mailbox-write: a debug write to
 * the mailbox counts as external for the doorbell. A debug write needs the
 * gdbstub (or a vCPU doing a virtual-address access) and aspfuzz_mailbox_write
 * is a host export; libqtest reaches neither. Always skipped, kept so the
 * gap shows up in the test list.
 */
static void test_doorbell_debug_write(void)
{
    g_test_skip("needs a debug-attribute write (gdbstub), not reachable from "
                "libqtest");
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);

    qtest_add_func("/amd-psp/pending-fixes/ccp/sha256-context-per-slot",
                   test_sha256_context_per_slot);
    qtest_add_func("/amd-psp/pending-fixes/ccp/sha256-continuation-no-context",
                   test_sha256_continuation_without_context);
    qtest_add_func("/amd-psp/pending-fixes/ccp/zlib-error-is-reported",
                   test_zlib_error_is_reported);
    qtest_add_func("/amd-psp/pending-fixes/ccp/reset", test_ccp_reset);
    qtest_add_func("/amd-psp/pending-fixes/timer/expiry-phase-across-restore",
                   test_timer_expiry_phase_across_restore);
    qtest_add_func("/amd-psp/pending-fixes/ic/enable-mask", test_ic_enable_mask);
    qtest_add_func("/amd-psp/pending-fixes/x86/port61-reset",
                   test_port61_reset);
    qtest_add_func("/amd-psp/pending-fixes/x86/port61-migrate",
                   test_port61_migrate);
    qtest_add_func("/amd-psp/pending-fixes/mailbox/doorbell-debug-write",
                   test_doorbell_debug_write);

    return psp_test_run();
}
