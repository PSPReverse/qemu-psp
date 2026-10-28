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

#define OBSERVER_PATH "/machine/soc/x86/access-observer"

static void observer_set(QTestState *qts, const char *prop, const char *val)
{
    qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments': "
                             "{'path': %s, 'property': %s, 'value': %s}}",
                             OBSERVER_PATH, prop, val);
}

static uint64_t observer_hits(QTestState *qts, const char *prop)
{
    QDict *rsp = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                           "{'path': %s, 'property': %s}}",
                           OBSERVER_PATH, prop);
    uint64_t v;

    g_assert_true(qdict_haskey(rsp, "return"));
    v = qdict_get_int(rsp, "return");
    qobject_unref(rsp);
    return v;
}

/*
 * The access observer sits behind the identity-mapped slots, so a PSP access
 * at X86_WINDOW0 + x is an x86 access at x. Window, exclude ranges and reset
 * are driven through the observer's write-only qtest properties.
 */
static char *observer_touched(QTestState *qts)
{
    QDict *rsp = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                           "{'path': %s, 'property': 'touched'}}",
                           OBSERVER_PATH);
    char *v;

    g_assert_true(qdict_haskey(rsp, "return"));
    v = g_strdup(qdict_get_str(rsp, "return"));
    qobject_unref(rsp);
    return v;
}

/* The pc is 0 here: the CPU does not run in qtest */
static void test_observer_touched(void)
{
    QTestState *qts = psp_startf(PSP_ZEN2,
        "-global driver=amd_psp.x86.mapper,property=use_access_observer,"
        "value=true");
    char *t;

    if (!qts) {
        return;
    }
    observer_set(qts, "window", "10000,200000");
    observer_set(qts, "exclude", "30000,1000");

    /* Outside the window and in an exclude range: nothing */
    qtest_readl(qts, X86_WINDOW0 + 0xfff0);
    qtest_readl(qts, X86_WINDOW0 + 0x210000);
    qtest_writel(qts, X86_WINDOW0 + 0x30010, 1);
    g_assert_cmpuint(observer_hits(qts, "touched_pages"), ==, 0);

    qtest_readl(qts, X86_WINDOW0 + 0x10008);
    qtest_readl(qts, X86_WINDOW0 + 0x10ff0);
    qtest_writel(qts, X86_WINDOW0 + 0x11004, 1);
    qtest_readl(qts, X86_WINDOW0 + 0x12000);
    qtest_writel(qts, X86_WINDOW0 + 0x12000, 1);
    qtest_writel(qts, X86_WINDOW0 + 0x13000, 1);
    qtest_readl(qts, X86_WINDOW0 + 0x13000);
    g_assert_cmpuint(observer_hits(qts, "touched_pages"), ==, 4);
    t = observer_touched(qts);
    g_assert_cmpstr(t, ==, "10000:1:0,11000:2:0,12000:3:0,13000:3:0");
    g_free(t);

    /* Activating clears the pages, reset too */
    observer_set(qts, "window", "10000,200000");
    g_assert_cmpuint(observer_hits(qts, "touched_pages"), ==, 0);
    qtest_readl(qts, X86_WINDOW0 + 0x10000);
    g_assert_cmpuint(observer_hits(qts, "touched_pages"), ==, 1);
    qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments': "
                             "{'path': %s, 'property': 'reset', "
                             "'value': true}}", OBSERVER_PATH);
    g_assert_cmpuint(observer_hits(qts, "touched_pages"), ==, 0);

    /* The 257th distinct page overflows; known pages still update */
    observer_set(qts, "window", "100000,200000");
    for (int i = 0; i < 257; i++) {
        qtest_readl(qts, X86_WINDOW0 + 0x100000 + i * 0x1000);
    }
    g_assert_cmpuint(observer_hits(qts, "touched_pages"), ==, 256);
    g_assert_cmpuint(observer_hits(qts, "touched_overflow"), ==, 1);
    qtest_writel(qts, X86_WINDOW0 + 0x100000, 1);
    g_assert_cmpuint(observer_hits(qts, "touched_overflow"), ==, 1);
    qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments': "
                             "{'path': %s, 'property': 'reset', "
                             "'value': true}}", OBSERVER_PATH);
    g_assert_cmpuint(observer_hits(qts, "touched_overflow"), ==, 0);
    qtest_quit(qts);
}

static void test_observer_hits(void)
{
    QTestState *qts = psp_startf(PSP_ZEN2,
        "-global driver=amd_psp.x86.mapper,property=use_access_observer,"
        "value=true");

    if (!qts) {
        return;
    }
    observer_set(qts, "window", "1000,100");
    observer_set(qts, "exclude", "1040,10");

    /* Outside the window, and inside an exclude range: not illegal */
    qtest_readl(qts, X86_WINDOW0 + 0xff0);
    qtest_readl(qts, X86_WINDOW0 + 0x1100);
    qtest_readl(qts, X86_WINDOW0 + 0x1048);
    qtest_writel(qts, X86_WINDOW0 + 0x1040, 1);
    g_assert_cmpuint(observer_hits(qts, "illegal_read_hits"), ==, 0);
    g_assert_cmpuint(observer_hits(qts, "illegal_write_hits"), ==, 0);

    /* Inside the window; the first hit of each kind is retained */
    qtest_readl(qts, X86_WINDOW0 + 0x1000);
    qtest_readl(qts, X86_WINDOW0 + 0x1020);
    qtest_writel(qts, X86_WINDOW0 + 0x1030, 1);
    qtest_writel(qts, X86_WINDOW0 + 0x1050, 1);
    qtest_writel(qts, X86_WINDOW0 + 0x1060, 1);
    g_assert_cmpuint(observer_hits(qts, "illegal_read_hits"), ==, 2);
    g_assert_cmpuint(observer_hits(qts, "illegal_write_hits"), ==, 3);
    g_assert_cmphex(observer_hits(qts, "illegal_read_addr"), ==, 0x1000);
    g_assert_cmphex(observer_hits(qts, "illegal_write_addr"), ==, 0x1030);

    /* Activating again drops the exclude ranges but keeps the results */
    observer_set(qts, "window", "1000,100");
    qtest_readl(qts, X86_WINDOW0 + 0x1048);
    g_assert_cmpuint(observer_hits(qts, "illegal_read_hits"), ==, 3);
    g_assert_cmphex(observer_hits(qts, "illegal_read_addr"), ==, 0x1000);

    /* Reset clears the counters and disarms the window */
    qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments': "
                             "{'path': %s, 'property': 'reset', "
                             "'value': true}}", OBSERVER_PATH);
    g_assert_cmpuint(observer_hits(qts, "illegal_read_hits"), ==, 0);
    g_assert_cmpuint(observer_hits(qts, "illegal_write_hits"), ==, 0);
    g_assert_cmphex(observer_hits(qts, "illegal_read_addr"), ==, 0);
    qtest_readl(qts, X86_WINDOW0 + 0x1000);
    g_assert_cmpuint(observer_hits(qts, "illegal_read_hits"), ==, 0);
    qtest_quit(qts);
}
/*
 * FCH UART 0 at x86 0xfedc9000 (4-byte register stride) reached through slot
 * 0: THR bytes land on the -serial chardev, LSR reports the transmitter idle.
 */
static void test_fch_uart(void)
{
    g_autofree char *path = NULL;
    g_autofree char *out = NULL;
    QTestState *qts;
    int fd = g_file_open_tmp("amd-psp-uart-XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    qts = psp_startf(PSP_ZEN2, "-serial file:%s", path);
    if (!qts) {
        unlink(path);
        return;
    }
    /* x86 0xfedc9000 is offset 0x2dc9000 into the 64 MiB slot at 0xfc000000 */
    qtest_writel(qts, X86_CTRL, FCH_UART_SLOT_BASE);
    g_assert_cmphex(qtest_readb(qts, FCH_UART_LSR) & 0x60, ==, 0x60);
    for (const char *c = "psp\n"; *c; c++) {
        qtest_writeb(qts, FCH_UART_THR, *c);
    }
    g_assert_cmphex(qtest_readb(qts, FCH_UART_LSR) & 0x60, ==, 0x60);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(path, &out, NULL, NULL));
    g_assert_cmpstr(out, ==, "psp\n");
    unlink(path);
}

/* Without a -serial chardev the UART still accepts writes and reports idle */
static void test_fch_uart_no_chardev(void)
{
    QTestState *qts = psp_startf(PSP_ZEN2, "-serial none");

    if (!qts) {
        return;
    }
    qtest_writel(qts, X86_CTRL, FCH_UART_SLOT_BASE);
    qtest_writeb(qts, FCH_UART_THR, 'x');
    g_assert_cmphex(qtest_readb(qts, FCH_UART_LSR) & 0x60, ==, 0x60);
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
    qtest_add_func("/amd-psp/x86/observer-hits", test_observer_hits);
    qtest_add_func("/amd-psp/x86/observer-touched", test_observer_touched);
    qtest_add_func("/amd-psp/x86/fch-uart", test_fch_uart);
    qtest_add_func("/amd-psp/x86/fch-uart-no-chardev",
                   test_fch_uart_no_chardev);
    qtest_add_func("/amd-psp/x86/observer-exec", test_observer_exec);

    return psp_test_run();
}
