/*
 * QTest for the AMD PSP x86 side: the slot mapper registers and the
 * placeholder I/O port 0x61 reached through a mapped slot. Reset and
 * migration of port 0x61 are pending fixes, see
 * amd-psp-pending-fixes-test.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "amd-psp-test.h"
#include <sys/resource.h>
#include <sys/wait.h>

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

/* An aborting child must die at once, not linger while dumping core */
static void no_core(gpointer unused)
{
    struct rlimit none = { 0, 0 };

    setrlimit(RLIMIT_CORE, &none);
}

/*
 * Executing out of the observer's I/O window must not trip the translator's
 * instruction-fetch recording. qtest does not run the CPU, so start a plain
 * TCG machine whose SRAM code does a bx into slot 0 of the x86 window (ARM
 * and Thumb target) and check that it is still alive a moment later.
 */
static void test_observer_exec(void)
{
    static const uint32_t targets[] = { X86_WINDOW0, X86_WINDOW0 | 1 };
    const char *bin = getenv("QTEST_QEMU_BINARY");

    if (psp_skip_reason) {
        g_test_skip(psp_skip_reason);
        return;
    }
    g_assert_nonnull(bin);

    for (int i = 0; i < ARRAY_SIZE(targets); i++) {
        g_autofree char *cmd = g_strdup_printf(
            "%s -machine " PSP_ZEN2 " -display none -monitor none "
            "-serial none -accel tcg "
            "-global driver=amd_psp.smnflash,property=flash_img,value=%s "
            "-global driver=amd_psp.x86.mapper,property=use_access_observer,"
            "value=true "
            /* ldr r1, [pc]; bx r1; .word target */
            "-device loader,addr=0x1000,data=0xe12fff11e59f1000,data-len=8,"
            "cpu-num=0 "
            "-device loader,addr=0x1008,data=0x%x,data-len=4",
            bin, psp_flash_path, targets[i]);
        g_auto(GStrv) argv = NULL;
        GPid pid;
        int status;

        g_assert_true(g_shell_parse_argv(cmd, NULL, &argv, NULL));
        g_assert_true(g_spawn_async(NULL, argv, NULL,
                                    G_SPAWN_DO_NOT_REAP_CHILD |
                                    G_SPAWN_STDOUT_TO_DEV_NULL |
                                    G_SPAWN_STDERR_TO_DEV_NULL,
                                    no_core, NULL, &pid, NULL));
        /* Still running after a few seconds: waitpid() sees no exit */
        for (int tick = 0; tick < 30; tick++) {
            g_usleep(G_USEC_PER_SEC / 10);
            g_assert_cmpint(waitpid(pid, &status, WNOHANG), ==, 0);
        }
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        g_spawn_close_pid(pid);
    }
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);

    qtest_add_func("/amd-psp/x86/mapper-registers", test_mapper_registers);
    qtest_add_func("/amd-psp/x86/port61", test_port61);
    qtest_add_func("/amd-psp/x86/observer-exec", test_observer_exec);

    return psp_test_run();
}
