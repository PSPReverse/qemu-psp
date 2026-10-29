/*
 * QTest for the AMD PSP CCP: queue registers, passthrough, SHA-256, zlib,
 * and the deterministic random number register. Descriptors are built from
 * the layout in include/psp/ccp/ccpv5-linux.h; data lives in SRAM.
 *
 * Behaviour that needs unmerged fixes (one SHA context per LSB slot, CCP
 * reset) is in amd-psp-pending-fixes-test.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "amd-psp-test.h"

#define Q(n) (CCP_BASE + 0x1000 * ((n) + 1))

/* A fresh queue is halted and idle; head and tail store, per queue */
static void test_queue_registers(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    for (int q = 0; q < 5; q++) {
        g_assert_cmphex(qtest_readl(qts, Q(q) + CCP_Q_CTRL), ==, CCP_Q_HALT);
        g_assert_cmphex(qtest_readl(qts, Q(q) + CCP_Q_HEAD), ==, 0);
        g_assert_cmphex(qtest_readl(qts, Q(q) + CCP_Q_TAIL), ==, 0);
        g_assert_cmphex(qtest_readl(qts, Q(q) + CCP_Q_STATUS), ==, 0);
    }
    qtest_writel(qts, Q(1) + CCP_Q_HEAD, 0x1234);
    qtest_writel(qts, Q(1) + CCP_Q_TAIL, 0x10);
    qtest_writel(qts, Q(3) + CCP_Q_TAIL, 0x20);
    g_assert_cmphex(qtest_readl(qts, Q(1) + CCP_Q_HEAD), ==, 0x1234);
    g_assert_cmphex(qtest_readl(qts, Q(1) + CCP_Q_TAIL), ==, 0x10);
    g_assert_cmphex(qtest_readl(qts, Q(3) + CCP_Q_TAIL), ==, 0x20);
    g_assert_cmphex(qtest_readl(qts, Q(0) + CCP_Q_HEAD), ==, 0);
    g_assert_cmphex(qtest_readl(qts, Q(3) + CCP_Q_HEAD), ==, 0);
    qtest_quit(qts);
}

/* Queue offset 0x11c is a plain word per queue, separate from its neighbours */
static void test_queue_scratch(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    for (int q = 0; q < 5; q++) {
        g_assert_cmphex(qtest_readl(qts, Q(q) + CCP_Q_SCRATCH), ==, 0);
    }
    for (int q = 0; q < 5; q++) {
        qtest_writel(qts, Q(q) + CCP_Q_SCRATCH, 0x76543210 + q);
    }
    for (int q = 0; q < 5; q++) {
        g_assert_cmphex(qtest_readl(qts, Q(q) + CCP_Q_SCRATCH), ==,
                        0x76543210 + q);
        g_assert_cmphex(qtest_readl(qts, Q(q) + CCP_Q_CTRL), ==, CCP_Q_HALT);
        g_assert_cmphex(qtest_readl(qts, Q(q) + CCP_Q_HEAD), ==, 0);
        g_assert_cmphex(qtest_readl(qts, Q(q) + CCP_Q_TAIL), ==, 0);
        g_assert_cmphex(qtest_readl(qts, Q(q) + CCP_Q_STATUS), ==, 0);
    }
    qtest_quit(qts);
}

/*
 * A passthrough copy lands, the queue ends halted with head == tail, the
 * completion is latched in the interrupt status and raised as IC source 0x15.
 */
static void test_passthrough(void)
{
    static const uint8_t data[] = "0123456789abcdefghijklmnopqrstu";
    QTestState *qts = psp_start(PSP_ZEN2);
    uint8_t out[sizeof(data)];
    bool pending[IC_SOURCES];

    if (!qts) {
        return;
    }
    qtest_memwrite(qts, SRAM_SRC, data, sizeof(data));
    g_assert_cmpuint(psp_ccp_copy(qts, SRAM_DST, SRAM_SRC, sizeof(data)),
                     ==, 0);
    qtest_memread(qts, SRAM_DST, out, sizeof(out));
    g_assert_cmpmem(out, sizeof(out), data, sizeof(data));

    g_assert_cmphex(qtest_readl(qts, CCP_Q0 + CCP_Q_CTRL), ==, CCP_Q_HALT);
    g_assert_cmphex(qtest_readl(qts, CCP_Q0 + CCP_Q_TAIL), ==,
                    qtest_readl(qts, CCP_Q0 + CCP_Q_HEAD));
    g_assert_cmphex(qtest_readl(qts, CCP_INT_STATUS), ==,
                    CCP_INT_STATUS_DONE);
    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, SRC_CCP);
    /* Write one to clear */
    qtest_writel(qts, CCP_INT_STATUS, CCP_INT_STATUS_DONE);
    g_assert_cmphex(qtest_readl(qts, CCP_INT_STATUS), ==, 0);
    qtest_quit(qts);
}

/* Through the LSB: a plain copy keeps the bytes, the 256-bit swap reverses */
static void test_passthrough_lsb(void)
{
    uint8_t data[32], out[32];
    QTestState *qts = psp_start(PSP_ZEN2);
    PspCcpDesc in = {
        .engine = CCP_ENGINE_PASSTHRU, .len = 32,
        .src = SRAM_SRC, .src_mem = CCP_MEMTYPE_LOCAL,
        .dst = 0x40, .dst_mem = CCP_MEMTYPE_SB,
    };
    PspCcpDesc plain = {
        .engine = CCP_ENGINE_PASSTHRU, .len = 32,
        .src = 0x40, .src_mem = CCP_MEMTYPE_SB,
        .dst = SRAM_DST, .dst_mem = CCP_MEMTYPE_LOCAL,
    };
    PspCcpDesc swapped = plain;

    if (!qts) {
        return;
    }
    swapped.flags = CCP_PT_BSWAP256;
    for (int i = 0; i < sizeof(data); i++) {
        data[i] = i * 7 + 1;
    }
    qtest_memwrite(qts, SRAM_SRC, data, sizeof(data));
    g_assert_cmpuint(psp_ccp_run(qts, &in), ==, 0);

    g_assert_cmpuint(psp_ccp_run(qts, &plain), ==, 0);
    qtest_memread(qts, SRAM_DST, out, sizeof(out));
    g_assert_cmpmem(out, sizeof(out), data, sizeof(data));

    g_assert_cmpuint(psp_ccp_run(qts, &swapped), ==, 0);
    qtest_memread(qts, SRAM_DST, out, sizeof(out));
    for (int i = 0; i < sizeof(out); i++) {
        g_assert_cmphex(out[i], ==, data[sizeof(data) - 1 - i]);
    }
    qtest_quit(qts);
}

/* SHA-256 over "abcd" with INIT and EOM succeeds and leaves the digest */
static void test_sha256(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    uint8_t want[32], got[32], raw[32];
    PspCcpDesc d = {
        .engine = CCP_ENGINE_PASSTHRU, .len = 32,
        .src = 0, .src_mem = CCP_MEMTYPE_SB,
        .dst = SRAM_DST, .dst_mem = CCP_MEMTYPE_LOCAL,
    };

    if (!qts) {
        return;
    }
    psp_expected_sha256("abcd", 4, want);
    qtest_memwrite(qts, SRAM_SRC, "abcd", 4);
    g_assert_cmpuint(psp_ccp_sha256(qts, SRAM_SRC, 4, 0, CCP_INIT | CCP_EOM),
                     ==, 0);
    psp_ccp_read_digest(qts, 0, got);
    g_assert_cmpmem(got, 32, want, 32);

    /* The CCP stores the digest reversed: the raw LSB bytes are */
    g_assert_cmpuint(psp_ccp_run(qts, &d), ==, 0);
    qtest_memread(qts, SRAM_DST, raw, sizeof(raw));
    for (int i = 0; i < 32; i++) {
        g_assert_cmphex(raw[i], ==, want[31 - i]);
    }
    qtest_quit(qts);
}

/* A hash split over two requests (INIT, then EOM) equals the one-shot hash */
static void test_sha256_split(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    uint8_t want[32], got[32];

    if (!qts) {
        return;
    }
    psp_expected_sha256("abcd", 4, want);
    qtest_memwrite(qts, SRAM_SRC, "abcd", 4);
    g_assert_cmpuint(psp_ccp_sha256(qts, SRAM_SRC, 2, 0, CCP_INIT), ==, 0);
    g_assert_cmpuint(psp_ccp_sha256(qts, SRAM_SRC + 2, 2, 0, CCP_EOM), ==, 0);
    psp_ccp_read_digest(qts, 0, got);
    g_assert_cmpmem(got, 32, want, 32);
    qtest_quit(qts);
}

/* INIT starts over; a later digest does not include earlier data */
static void test_sha256_init_restarts(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    uint8_t first[32], second[32], want[32];

    if (!qts) {
        return;
    }
    qtest_memwrite(qts, SRAM_SRC, "abcd", 4);
    qtest_memwrite(qts, SRAM_SRC + 0x100, "wxyz", 4);
    g_assert_cmpuint(psp_ccp_sha256(qts, SRAM_SRC, 4, 0, CCP_INIT | CCP_EOM),
                     ==, 0);
    psp_ccp_read_digest(qts, 0, first);
    g_assert_cmpuint(psp_ccp_sha256(qts, SRAM_SRC + 0x100, 4, 0,
                                    CCP_INIT | CCP_EOM), ==, 0);
    psp_ccp_read_digest(qts, 0, second);
    psp_expected_sha256("wxyz", 4, want);
    g_assert_cmpmem(second, 32, want, 32);
    g_assert_true(memcmp(first, second, 32) != 0);
    qtest_quit(qts);
}

/* A valid zlib stream inflates and the counter reports the output size */
static void test_zlib(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    char out[4];

    if (!qts) {
        return;
    }
    g_assert_cmpuint(qtest_readl(qts, CCP_ZLIB_WRITTEN), ==, 0);
    qtest_memwrite(qts, SRAM_SRC, psp_zlib_abcd, sizeof(psp_zlib_abcd));
    g_assert_cmpuint(psp_ccp_zlib(qts, sizeof(psp_zlib_abcd)), ==, 0);
    qtest_memread(qts, SRAM_DST, out, sizeof(out));
    g_assert_cmpmem(out, sizeof(out), "abcd", 4);
    g_assert_cmpuint(qtest_readl(qts, CCP_ZLIB_WRITTEN), ==, 4);
    qtest_quit(qts);
}

/*
 * A stream that is not zlib fails inside the engine; the device carries on:
 * the next copy, and the next valid stream, work. The status of the failed
 * request itself is not asserted: that is the pending-fixes group's business.
 */
static void test_zlib_error_leaves_device_usable(void)
{
    static const uint8_t data[] = "after the error";
    QTestState *qts = psp_start(PSP_ZEN2);
    uint8_t garbage[16];
    uint8_t out[sizeof(data)];
    char abcd[4];

    if (!qts) {
        return;
    }
    memset(garbage, 0xff, sizeof(garbage));
    qtest_memwrite(qts, SRAM_SRC, garbage, sizeof(garbage));
    psp_ccp_zlib(qts, sizeof(garbage));
    g_assert_cmpuint(qtest_readl(qts, CCP_ZLIB_WRITTEN), ==, 0);

    qtest_memwrite(qts, SRAM_SRC, data, sizeof(data));
    g_assert_cmpuint(psp_ccp_copy(qts, SRAM_DST, SRAM_SRC, sizeof(data)),
                     ==, 0);
    qtest_memread(qts, SRAM_DST, out, sizeof(out));
    g_assert_cmpmem(out, sizeof(out), data, sizeof(data));

    qtest_memwrite(qts, SRAM_SRC, psp_zlib_abcd, sizeof(psp_zlib_abcd));
    g_assert_cmpuint(psp_ccp_zlib(qts, sizeof(psp_zlib_abcd)), ==, 0);
    qtest_memread(qts, SRAM_DST, abcd, sizeof(abcd));
    g_assert_cmpmem(abcd, sizeof(abcd), "abcd", 4);
    g_assert_cmpuint(qtest_readl(qts, CCP_ZLIB_WRITTEN), ==, 4);
    qtest_quit(qts);
}

#define TRNG_WORDS 8

static void trng_draw(QTestState *qts, uint32_t w[TRNG_WORDS])
{
    for (int i = 0; i < TRNG_WORDS; i++) {
        w[i] = qtest_readl(qts, CCP_TRNG);
    }
}

/* The default seed is fixed: runs are reproducible. Words are never 0 and
 * never repeat the previous one, as the firmware requires. */
static void test_trng_default_seed(void)
{
    uint32_t a[TRNG_WORDS], b[TRNG_WORDS], c[TRNG_WORDS];
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    trng_draw(qts, a);
    qtest_quit(qts);
    for (int i = 0; i < TRNG_WORDS; i++) {
        g_assert_cmphex(a[i], !=, 0);
        if (i) {
            g_assert_cmphex(a[i], !=, a[i - 1]);
        }
    }
    qts = psp_start(PSP_ZEN2);
    trng_draw(qts, b);
    qtest_quit(qts);
    g_assert_cmpmem(a, sizeof(a), b, sizeof(b));

    /* Other generations share the CCP model */
    qts = psp_start(PSP_ZEN3);
    trng_draw(qts, c);
    qtest_quit(qts);
    g_assert_cmpmem(a, sizeof(a), c, sizeof(c));
}

/* The sequence is a function of the trng_seed property */
static void test_trng_seed(void)
{
    uint32_t a[TRNG_WORDS], b[TRNG_WORDS], c[TRNG_WORDS], d[TRNG_WORDS];
    QTestState *qts;

    qts = psp_startf(PSP_ZEN2, "-global driver=amd.ccpV5,property=trng_seed,"
                     "value=1234");
    if (!qts) {
        return;
    }
    trng_draw(qts, a);
    qtest_quit(qts);
    qts = psp_startf(PSP_ZEN2, "-global driver=amd.ccpV5,property=trng_seed,"
                     "value=1234");
    trng_draw(qts, b);
    qtest_quit(qts);
    g_assert_cmpmem(a, sizeof(a), b, sizeof(b));

    qts = psp_startf(PSP_ZEN2, "-global driver=amd.ccpV5,property=trng_seed,"
                     "value=4321");
    trng_draw(qts, c);
    qtest_quit(qts);
    g_assert_true(memcmp(a, c, sizeof(a)) != 0);
    g_assert_cmphex(a[0], !=, c[0]);

    qts = psp_start(PSP_ZEN2);
    trng_draw(qts, d);
    qtest_quit(qts);
    g_assert_true(memcmp(a, d, sizeof(a)) != 0);
}

/* A byte or halfword read shows the last word drawn and does not draw */
static void test_trng_narrow_read(void)
{
    uint32_t ref[3], w;
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        ref[i] = qtest_readl(qts, CCP_TRNG);
    }
    qtest_quit(qts);

    qts = psp_start(PSP_ZEN2);
    g_assert_cmphex(qtest_readb(qts, CCP_TRNG), ==, 0);
    g_assert_cmphex(qtest_readl(qts, CCP_TRNG), ==, ref[0]);
    g_assert_cmphex(qtest_readb(qts, CCP_TRNG), ==, ref[0] & 0xff);
    g_assert_cmphex(qtest_readb(qts, CCP_TRNG + 3), ==, ref[0] >> 24);
    g_assert_cmphex(qtest_readw(qts, CCP_TRNG + 2), ==, ref[0] >> 16);
    w = qtest_readl(qts, CCP_TRNG);
    g_assert_cmphex(w, ==, ref[1]);
    g_assert_cmphex(qtest_readl(qts, CCP_TRNG), ==, ref[2]);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);

    qtest_add_func("/amd-psp/ccp/queue-registers", test_queue_registers);
    qtest_add_func("/amd-psp/ccp/queue-scratch", test_queue_scratch);
    qtest_add_func("/amd-psp/ccp/passthrough", test_passthrough);
    qtest_add_func("/amd-psp/ccp/passthrough-lsb", test_passthrough_lsb);
    qtest_add_func("/amd-psp/ccp/sha256", test_sha256);
    qtest_add_func("/amd-psp/ccp/sha256-split", test_sha256_split);
    qtest_add_func("/amd-psp/ccp/sha256-init-restarts",
                   test_sha256_init_restarts);
    qtest_add_func("/amd-psp/ccp/zlib", test_zlib);
    qtest_add_func("/amd-psp/ccp/zlib-error-leaves-device-usable",
                   test_zlib_error_leaves_device_usable);
    qtest_add_func("/amd-psp/ccp/trng-default-seed", test_trng_default_seed);
    qtest_add_func("/amd-psp/ccp/trng-seed", test_trng_seed);
    qtest_add_func("/amd-psp/ccp/trng-narrow-read", test_trng_narrow_read);

    return psp_test_run();
}
