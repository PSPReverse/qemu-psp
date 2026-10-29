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

#define SMN_CTRL 0x03220000
#define SMN_WINDOW0 0x01000000          /* PSP address of slot 0 */
#define SMN_SLOT_SIZE 0x100000
#define X86_SLOT_SIZE 0x4000000

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

/* The regular DRAM defaults to 8 GiB on Zen3 and 1 GiB elsewhere */
static void test_regular_dram_default(void)
{
    static const struct {
        const char *machine;
        uint64_t size;
    } cases[] = {
        { PSP_ZEN2, 1ULL << 30 },
        { PSP_ZEN3, 8ULL << 30 },
    };

    for (int i = 0; i < G_N_ELEMENTS(cases); i++) {
        QTestState *qts = psp_start(cases[i].machine);
        QDict *rsp;

        if (!qts) {
            return;
        }
        rsp = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                        "{'path': '/machine/soc/x86/x86-full-memory', "
                        "'property': 'regular_dram_size'}}");
        g_assert_true(qdict_haskey(rsp, "return"));
        g_assert_cmpuint(qdict_get_int(rsp, "return"), ==, cases[i].size);
        qobject_unref(rsp);
        qtest_quit(qts);
    }
}

/*
 * The Zen3 DRAM is split around the MMIO hole: the first 2 GiB below it, the
 * rest above 4 GiB. The FCH UART below 4 GiB stays reachable, both DRAM parts
 * are accessible and they are backed by the same RAM block.
 */
static void test_zen3_dram_split(void)
{
    QTestState *qts = psp_start(PSP_ZEN3);

    if (!qts) {
        return;
    }
    /* Slot 0 on the last 64 MiB of low DRAM (x86 0x7c000000) */
    qtest_writel(qts, X86_CTRL, 0x1f);
    qtest_writel(qts, X86_WINDOW0 + 0x100, 0x11223344);
    g_assert_cmphex(qtest_readl(qts, X86_WINDOW0 + 0x100), ==, 0x11223344);
    /* Slot 0 on the first 64 MiB above 4 GiB (x86 0x100000000) */
    qtest_writel(qts, X86_CTRL, 1u << 6);
    g_assert_cmphex(qtest_readl(qts, X86_WINDOW0 + 0x100), ==, 0);
    qtest_writel(qts, X86_WINDOW0 + 0x100, 0x55667788);
    g_assert_cmphex(qtest_readl(qts, X86_WINDOW0 + 0x100), ==, 0x55667788);
    /* The last 64 MiB of the 6 GiB above 4 GiB (x86 0x27c000000) */
    qtest_writel(qts, X86_CTRL, (2u << 6) | 0x1f);
    qtest_writel(qts, X86_WINDOW0 + 0x100, 0x99aabbcc);
    g_assert_cmphex(qtest_readl(qts, X86_WINDOW0 + 0x100), ==, 0x99aabbcc);
    /* Nothing of the above aliased into low DRAM */
    qtest_writel(qts, X86_CTRL, 0x1f);
    g_assert_cmphex(qtest_readl(qts, X86_WINDOW0 + 0x100), ==, 0x11223344);
    /* The FCH UART below 4 GiB is not shadowed by DRAM */
    qtest_writel(qts, X86_CTRL, FCH_UART_SLOT_BASE);
    g_assert_cmphex(qtest_readb(qts, FCH_UART_LSR) & 0x60, ==, 0x60);
    qtest_quit(qts);
}

/*
 * A window shows what its slot register selects, at every access size, also
 * when it is used a lot while it stays put and when it is moved afterwards.
 */
static void test_window_retarget(void)
{
    QTestState *qts = psp_start(PSP_ZEN3);
    static const uint8_t want[7] = { 0x00, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };
    uint8_t buf[7];

    if (!qts) {
        return;
    }
    /* Slot 0 on low DRAM (x86 0x0) */
    qtest_writel(qts, X86_CTRL, 0);
    qtest_writeb(qts, X86_WINDOW0 + 0x100, 0x11);
    qtest_writew(qts, X86_WINDOW0 + 0x102, 0x3322);
    qtest_writel(qts, X86_WINDOW0 + 0x104, 0x77665544);
    qtest_writeq(qts, X86_WINDOW0 + 0x108, 0xffeeddccbbaa9988ull);
    g_assert_cmphex(qtest_readq(qts, X86_WINDOW0 + 0x100), ==,
                    0x7766554433220011ull);
    g_assert_cmphex(qtest_readb(qts, X86_WINDOW0 + 0x100), ==, 0x11);
    g_assert_cmphex(qtest_readw(qts, X86_WINDOW0 + 0x102), ==, 0x3322);
    g_assert_cmphex(qtest_readl(qts, X86_WINDOW0 + 0x104), ==, 0x77665544);
    g_assert_cmphex(qtest_readq(qts, X86_WINDOW0 + 0x108), ==,
                    0xffeeddccbbaa9988ull);
    /* Unaligned and crossing words */
    qtest_memread(qts, X86_WINDOW0 + 0x101, buf, sizeof(buf));
    g_assert_cmpmem(buf, sizeof(buf), want, sizeof(want));

    /* A move shows other memory, and the memory behind the old one is kept */
    qtest_writel(qts, X86_CTRL, 1u << 6);
    g_assert_cmphex(qtest_readl(qts, X86_WINDOW0 + 0x104), ==, 0);
    qtest_writel(qts, X86_WINDOW0 + 0x104, 0xdeadbeef);
    qtest_writel(qts, X86_CTRL, 0);
    g_assert_cmphex(qtest_readl(qts, X86_WINDOW0 + 0x104), ==, 0x77665544);

    /* Heavy use makes the window an alias; it must show the same memory */
    for (int i = 0; i < 1000; i++) {
        qtest_writel(qts, X86_WINDOW0 + 0x1000 + 4 * i, i * 3 + 1);
    }
    for (int i = 0; i < 1000; i++) {
        g_assert_cmphex(qtest_readl(qts, X86_WINDOW0 + 0x1000 + 4 * i), ==,
                        i * 3 + 1);
    }
    /* Moving it back and forth keeps both contents */
    qtest_writel(qts, X86_CTRL, 1u << 6);
    g_assert_cmphex(qtest_readl(qts, X86_WINDOW0 + 0x1000 + 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, X86_WINDOW0 + 0x104), ==, 0xdeadbeef);
    qtest_writel(qts, X86_CTRL, 0);
    for (int i = 0; i < 1000; i++) {
        g_assert_cmphex(qtest_readl(qts, X86_WINDOW0 + 0x1000 + 4 * i), ==,
                        i * 3 + 1);
    }
    qtest_quit(qts);
}

/*
 * A window onto a device keeps reaching it when moved to RAM and back, also
 * after many accesses, and a window onto nothing mapped reads as such.
 */
static void test_window_device(void)
{
    QTestState *qts = psp_start(PSP_ZEN3);

    if (!qts) {
        return;
    }
    qtest_writel(qts, X86_CTRL, FCH_UART_SLOT_BASE);
    for (int i = 0; i < 1000; i++) {
        g_assert_cmphex(qtest_readb(qts, FCH_UART_LSR) & 0x60, ==, 0x60);
    }
    qtest_writel(qts, X86_CTRL, 0x1f);
    qtest_writel(qts, X86_WINDOW0 + 0x100, 0x11223344);
    qtest_writel(qts, X86_CTRL, FCH_UART_SLOT_BASE);
    g_assert_cmphex(qtest_readb(qts, FCH_UART_LSR) & 0x60, ==, 0x60);
    qtest_writel(qts, X86_CTRL, 0x1f);
    g_assert_cmphex(qtest_readl(qts, X86_WINDOW0 + 0x100), ==, 0x11223344);
    qtest_quit(qts);
}

/* zlib stream of PAYLOAD in one stored block */
static size_t zlib_stored(uint8_t *out, const uint8_t *payload, uint16_t len)
{
    uint32_t a = 1, b = 0;
    size_t n = 0;

    out[n++] = 0x78;
    out[n++] = 0x01;
    out[n++] = 0x01;
    out[n++] = len & 0xff;
    out[n++] = len >> 8;
    out[n++] = ~len & 0xff;
    out[n++] = (~len >> 8) & 0xff;
    for (int i = 0; i < len; i++) {
        a = (a + payload[i]) % 65521;
        b = (b + a) % 65521;
        out[n++] = payload[i];
    }
    out[n++] = b >> 8;
    out[n++] = b & 0xff;
    out[n++] = a >> 8;
    out[n++] = a & 0xff;
    return n;
}

/*
 * The CCP inflates a stream that it must map whole. It is longer than a bounce
 * buffer, and sits in RAM behind a window that was only just pointed there.
 */
static void test_window_ccp(void)
{
    enum { PAYLOAD = 6000 };
    uint8_t payload[PAYLOAD], stream[PAYLOAD + 16], out[PAYLOAD];
    size_t n;

    for (int i = 0; i < PAYLOAD; i++) {
        payload[i] = i * 13 + (i >> 8);
    }
    n = zlib_stored(stream, payload, PAYLOAD);

    for (int c = 0; c < 3; c++) {
        QTestState *qts = psp_start(PSP_ZEN3);
        PspCcpDesc d = {
            .engine = CCP_ENGINE_ZLIB, .flags = CCP_INIT | CCP_EOM,
            .len = n, .src_mem = CCP_MEMTYPE_LOCAL,
            .dst = SRAM_DST, .dst_mem = CCP_MEMTYPE_LOCAL,
        };

        uint64_t out_addr = SRAM_DST;

        if (!qts) {
            return;
        }
        if (c == 0) {
            /*
             * x86 DRAM at x86 0x7c000000 through slots 0 and 1. The data goes
             * in through slot 1, so slot 0 is not used before the CCP is.
             */
            qtest_writel(qts, X86_CTRL + X86_SLOT_REGS, 0x1f);
            qtest_memwrite(qts, X86_WINDOW0 + X86_SLOT_SIZE + 0x1000, stream,
                           n);
            qtest_writel(qts, X86_CTRL, 0x1f);
            d.src = X86_WINDOW0 + 0x1000;
        } else if (c == 1) {
            /* The 1 MiB SMN RAM window at 0x30500000 through SMN slots 0, 1 */
            qtest_writew(qts, SMN_CTRL + 2, 0x305);
            qtest_memwrite(qts, SMN_WINDOW0 + SMN_SLOT_SIZE + 0x1000, stream,
                           n);
            qtest_writew(qts, SMN_CTRL, 0x305);
            d.src = SMN_WINDOW0 + 0x1000;
        } else {
            /*
             * Secure DRAM in the tail of an x86 window that starts in MMIO,
             * the Zen3 BL's layout: the target is no RAM at its start, and
             * the stream is longer than a bounce buffer.
             */
            qtest_writel(qts, X86_CTRL + X86_SLOT_REGS, 0x3fff7e);
            qtest_memwrite(qts, X86_WINDOW0 + X86_SLOT_SIZE + 0x3820760,
                           stream, n);
            qtest_writel(qts, X86_CTRL, 0x3fff7e);
            d.src = X86_WINDOW0 + 0x3820760;
            d.dst = X86_WINDOW0 + 0x3800000;
            out_addr = d.dst;
        }
        g_assert_cmpuint(psp_ccp_run(qts, &d), ==, 0);
        g_assert_cmpuint(qtest_readl(qts, CCP_ZLIB_WRITTEN), ==, PAYLOAD);
        qtest_memread(qts, out_addr, out, sizeof(out));
        g_assert_cmpmem(out, sizeof(out), payload, sizeof(payload));
        qtest_quit(qts);
    }
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);

    qtest_add_func("/amd-psp/x86/mapper-registers", test_mapper_registers);
    qtest_add_func("/amd-psp/x86/regular-dram-default",
                   test_regular_dram_default);
    qtest_add_func("/amd-psp/x86/zen3-dram-split", test_zen3_dram_split);
    qtest_add_func("/amd-psp/x86/window-retarget", test_window_retarget);
    qtest_add_func("/amd-psp/x86/window-device", test_window_device);
    qtest_add_func("/amd-psp/x86/window-ccp", test_window_ccp);
    qtest_add_func("/amd-psp/x86/port61", test_port61);
    qtest_add_func("/amd-psp/x86/observer-hits", test_observer_hits);
    qtest_add_func("/amd-psp/x86/observer-touched", test_observer_touched);
    qtest_add_func("/amd-psp/x86/fch-uart", test_fch_uart);
    qtest_add_func("/amd-psp/x86/fch-uart-no-chardev",
                   test_fch_uart_no_chardev);
    qtest_add_func("/amd-psp/x86/observer-exec", test_observer_exec);

    return psp_test_run();
}
