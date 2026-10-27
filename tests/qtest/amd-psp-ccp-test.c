/*
 * QTest for the AMD PSP CCP: queue registers, passthrough, SHA-256/384, RSA, zlib,
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

/* SHA-384 over "abcd" (48-byte digest, stored reversed in the LSB slot) */
static void test_sha384(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    uint8_t want[48], raw[48];
    gsize outlen = sizeof(want);
    GChecksum *c = g_checksum_new(G_CHECKSUM_SHA384);
    PspCcpDesc sha = {
        .engine = CCP_ENGINE_SHA, .flags = CCP_SHA384 | CCP_INIT | CCP_EOM,
        .len = 4, .src = SRAM_SRC, .src_mem = CCP_MEMTYPE_LOCAL, .dst = 0x30,
    };
    PspCcpDesc rd = {
        .engine = CCP_ENGINE_PASSTHRU, .len = 48,
        .src = 0, .src_mem = CCP_MEMTYPE_SB,
        .dst = SRAM_DST, .dst_mem = CCP_MEMTYPE_LOCAL,
    };

    g_checksum_update(c, (const guchar *)"abcd", 4);
    g_checksum_get_digest(c, want, &outlen);
    g_checksum_free(c);
    if (!qts) {
        return;
    }
    qtest_memwrite(qts, SRAM_SRC, "abcd", 4);
    g_assert_cmpuint(psp_ccp_run(qts, &sha), ==, 0);
    g_assert_cmpuint(psp_ccp_run(qts, &rd), ==, 0);
    qtest_memread(qts, SRAM_DST, raw, sizeof(raw));
    for (int i = 0; i < 48; i++) {
        g_assert_cmphex(raw[i], ==, want[47 - i]);
    }
    qtest_quit(qts);
}

/*
 * RSA public operation, little-endian operands: the source holds the modulus
 * followed by the message, the key address holds the exponent. Vector from
 * python3 pow(m, 65537, n) for a random 2048-bit n.
 */
static const uint8_t rsa_n[256] = {
    0x4b, 0x38, 0xa4, 0xce, 0x91, 0x5f, 0x73, 0x75, 0xe6, 0xaf, 0x65, 0x0d,
    0xee, 0x12, 0x59, 0x28, 0xcb, 0x52, 0xcd, 0x87, 0x96, 0x20, 0x64, 0xbe,
    0x3d, 0xf5, 0xfd, 0x3b, 0x10, 0x45, 0x3f, 0x50, 0xe1, 0xd5, 0xc0, 0x98,
    0x45, 0x42, 0xeb, 0xa3, 0x96, 0x19, 0xd6, 0xff, 0x96, 0x7c, 0xdc, 0x91,
    0x41, 0x48, 0xb8, 0x07, 0x9c, 0xcd, 0x6a, 0x65, 0x41, 0x9e, 0xbf, 0x9e,
    0x0c, 0xa9, 0x11, 0x74, 0xbc, 0x17, 0xa0, 0x54, 0xe3, 0x6e, 0xc6, 0x0f,
    0xb7, 0x76, 0x08, 0xed, 0x67, 0xe0, 0xc2, 0x72, 0x64, 0xac, 0x37, 0x5f,
    0x2c, 0x06, 0x2c, 0x58, 0x40, 0xa1, 0x95, 0xa2, 0xfb, 0x3c, 0x64, 0xd4,
    0x51, 0x6a, 0x13, 0xce, 0xc3, 0xfe, 0x81, 0xb1, 0x06, 0xa3, 0x70, 0x2f,
    0x7c, 0xb6, 0xa3, 0x06, 0x7a, 0x76, 0xf1, 0xf3, 0xc0, 0x7b, 0xe9, 0x14,
    0xd9, 0x1c, 0x42, 0x3c, 0x9f, 0x80, 0xb9, 0x42, 0x4e, 0x2d, 0xf1, 0xeb,
    0x1e, 0xf9, 0xd4, 0x08, 0x18, 0xc2, 0x9d, 0xd1, 0x03, 0x30, 0xe4, 0x35,
    0x5d, 0x0d, 0x35, 0x99, 0x83, 0xa0, 0x9f, 0x4a, 0xe6, 0xcf, 0x9f, 0x69,
    0xd9, 0x24, 0x64, 0xc5, 0x4f, 0x96, 0x9a, 0x62, 0x55, 0xf6, 0x87, 0xc0,
    0xb4, 0x26, 0x0d, 0x1f, 0x0c, 0xf2, 0xae, 0xfb, 0x55, 0x58, 0xfb, 0x8a,
    0x6f, 0xc2, 0xa7, 0xf0, 0x29, 0x0b, 0xd2, 0x9e, 0x96, 0x18, 0xd8, 0x52,
    0x8c, 0xd1, 0x01, 0x4e, 0xe5, 0x37, 0xf6, 0x58, 0xfa, 0x55, 0x36, 0x42,
    0xc3, 0x57, 0x7d, 0x44, 0x8d, 0x5b, 0xba, 0xbc, 0x3c, 0xa7, 0x91, 0x96,
    0xe5, 0x12, 0xee, 0xdc, 0xb9, 0x1c, 0x65, 0x4d, 0x03, 0x62, 0x18, 0xad,
    0xdd, 0x52, 0x61, 0x9f, 0xd7, 0x4d, 0xe6, 0x1a, 0xf4, 0x7a, 0xde, 0xeb,
    0xbf, 0x48, 0xf0, 0xc1, 0xa2, 0xdd, 0x8a, 0xd8, 0xe8, 0xaf, 0xa2, 0x47,
    0x2d, 0x97, 0x3a, 0xb6,
};
static const uint8_t rsa_m[256] = {
    0x20, 0x4d, 0x1e, 0x25, 0x1d, 0xd5, 0x45, 0xf4, 0x00, 0x2a, 0x72, 0x3e,
    0x79, 0x51, 0x1d, 0x3c, 0xeb, 0x9b, 0x1d, 0x37, 0x3f, 0x8a, 0x9f, 0xbc,
    0x94, 0x94, 0x61, 0x5a, 0xdf, 0x90, 0xce, 0x3a, 0x58, 0xc1, 0x8f, 0x99,
    0x95, 0x5f, 0xd0, 0xcf, 0x35, 0xe9, 0x1e, 0x9f, 0x25, 0x20, 0xc6, 0xb1,
    0x80, 0xd6, 0x6b, 0x54, 0xe0, 0x2f, 0xf8, 0x1f, 0x96, 0x23, 0x29, 0xb5,
    0x1c, 0x59, 0xe6, 0x52, 0x97, 0xc7, 0xef, 0xb8, 0xde, 0xab, 0x00, 0x9a,
    0x63, 0xa0, 0xde, 0xf5, 0xb9, 0xb0, 0x0d, 0x22, 0x04, 0xa0, 0x1f, 0x85,
    0x10, 0x1f, 0x43, 0x45, 0x0c, 0x68, 0x9b, 0x24, 0x85, 0xaa, 0x3d, 0x70,
    0xc3, 0xce, 0x5a, 0x59, 0x69, 0xd4, 0xde, 0xc4, 0xa0, 0xc7, 0x46, 0x45,
    0x01, 0xa1, 0x77, 0xea, 0xe5, 0x19, 0x97, 0x66, 0x02, 0xbe, 0xd8, 0xe7,
    0x42, 0x0a, 0x87, 0xf6, 0xaa, 0x73, 0x2d, 0x0b, 0x1d, 0xaf, 0xab, 0x75,
    0xf5, 0x11, 0x08, 0xbe, 0x10, 0x7e, 0x5a, 0x82, 0x08, 0x33, 0x19, 0x43,
    0xf6, 0xe8, 0x4c, 0x6f, 0x0a, 0x90, 0x4d, 0x9f, 0x16, 0x78, 0x36, 0x52,
    0xa0, 0x94, 0xfa, 0x08, 0xe6, 0x95, 0x54, 0xab, 0x2d, 0x60, 0x3f, 0xc2,
    0x5f, 0xad, 0xbf, 0xe1, 0x2e, 0x8b, 0x9c, 0xa3, 0x95, 0xbc, 0xcb, 0xcb,
    0xa4, 0x24, 0xcd, 0xf4, 0x28, 0x3d, 0x6f, 0x5c, 0x3e, 0xcc, 0xf2, 0x4c,
    0x64, 0x0b, 0x46, 0xee, 0x43, 0x93, 0x25, 0xcb, 0x74, 0x95, 0xf7, 0x39,
    0xe8, 0x8d, 0x87, 0xbc, 0xd2, 0xe5, 0x67, 0x0c, 0x0a, 0x26, 0x02, 0xfc,
    0xcb, 0x06, 0xe3, 0xbe, 0x0a, 0xa0, 0x81, 0xa4, 0xab, 0x26, 0x2e, 0x0c,
    0x9d, 0x58, 0x2a, 0x84, 0x57, 0xfc, 0x3a, 0xab, 0xa1, 0x7b, 0x3a, 0x32,
    0xa3, 0x88, 0x7f, 0xa5, 0xce, 0xcb, 0xc2, 0xee, 0x5c, 0x3e, 0xbc, 0x43,
    0x42, 0x4a, 0x35, 0x00,
};
static const uint8_t rsa_c[256] = {
    0xca, 0x90, 0xdc, 0xc4, 0xf0, 0x66, 0x0e, 0x58, 0x59, 0x33, 0xca, 0x83,
    0x7b, 0x36, 0x44, 0x37, 0xf8, 0x52, 0x8c, 0xbb, 0x7a, 0xc2, 0x20, 0xbc,
    0x25, 0xd2, 0x13, 0x0a, 0xdb, 0x21, 0x1b, 0x00, 0x73, 0x8f, 0xd8, 0x06,
    0x46, 0xcf, 0xc7, 0x06, 0x6b, 0x02, 0xbb, 0x08, 0x7b, 0x36, 0x71, 0x89,
    0x69, 0x7e, 0xca, 0xdf, 0x80, 0x2d, 0x88, 0x78, 0xf1, 0xfc, 0x60, 0x08,
    0x2e, 0x55, 0xd5, 0x03, 0x15, 0xe5, 0x78, 0x8c, 0xb7, 0x71, 0x63, 0x7e,
    0x84, 0x77, 0xdf, 0x90, 0xb2, 0x12, 0x77, 0x58, 0x8f, 0xa4, 0x62, 0x27,
    0x8f, 0xae, 0xfa, 0xb9, 0x08, 0xd4, 0x46, 0x44, 0x53, 0xe1, 0xa8, 0xb9,
    0x18, 0xaa, 0x19, 0xc2, 0xd3, 0x61, 0x61, 0x6b, 0xad, 0x6e, 0x36, 0x34,
    0xb0, 0xa3, 0xc2, 0x55, 0xae, 0xb2, 0xaf, 0x1c, 0xba, 0xd8, 0xfd, 0x36,
    0x61, 0x9a, 0x47, 0x19, 0x02, 0x66, 0x4d, 0x01, 0xc2, 0x64, 0xa0, 0x2f,
    0x32, 0xe5, 0xcb, 0x32, 0x12, 0x6d, 0x2a, 0x39, 0xde, 0x88, 0xff, 0xec,
    0xd9, 0x92, 0x97, 0x8d, 0xcf, 0xd8, 0x9a, 0xcb, 0x3b, 0xb7, 0x6f, 0x87,
    0xd1, 0x19, 0x8c, 0xcc, 0xaa, 0xe9, 0xee, 0xea, 0xbf, 0xc4, 0x75, 0x33,
    0x35, 0x9b, 0xe0, 0x98, 0x26, 0x59, 0x45, 0xad, 0xea, 0x55, 0xf0, 0x42,
    0x34, 0x4c, 0xe8, 0x59, 0xef, 0xf2, 0xbb, 0x24, 0x0a, 0x11, 0xd7, 0xc4,
    0x7e, 0xcd, 0xf8, 0x59, 0x53, 0x89, 0x9a, 0x95, 0x97, 0xea, 0x7d, 0x10,
    0x66, 0xa7, 0xaa, 0x86, 0x2e, 0x27, 0x56, 0xd3, 0xab, 0x20, 0x17, 0x41,
    0xce, 0x15, 0xc3, 0x10, 0x11, 0xe6, 0x46, 0x55, 0x4d, 0x11, 0xc1, 0xa8,
    0x78, 0x7f, 0x33, 0x16, 0xb0, 0xce, 0xef, 0xdd, 0x7a, 0xaa, 0x0c, 0x73,
    0x1f, 0xfd, 0xd2, 0x12, 0xea, 0x4b, 0xa9, 0x23, 0x60, 0xc3, 0xa1, 0xe3,
    0xb9, 0xc2, 0x92, 0x1a,
};

static void psp_rsa_desc(PspCcpDesc *d, unsigned bytes, unsigned mode,
                         unsigned key_mem)
{
    *d = (PspCcpDesc){
        .engine = CCP_ENGINE_RSA, .flags = CCP_FUNC(bytes << 3 | mode),
        .len = 2 * bytes, .src = SRAM_SRC, .src_mem = CCP_MEMTYPE_LOCAL,
        .dst = SRAM_DST, .dst_mem = CCP_MEMTYPE_LOCAL,
        .key = key_mem == CCP_MEMTYPE_SB ? 0 : SRAM_KEY, .key_mem = key_mem,
    };
}

static void test_rsa_2048(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    uint8_t exp[256] = { 0x01, 0x00, 0x01 }, out[256];
    PspCcpDesc d;

    if (!qts) {
        return;
    }
    psp_rsa_desc(&d, 256, 0, CCP_MEMTYPE_LOCAL);
    qtest_memwrite(qts, SRAM_SRC, rsa_n, 256);
    qtest_memwrite(qts, SRAM_SRC + 256, rsa_m, 256);
    qtest_memwrite(qts, SRAM_KEY, exp, 256);
    g_assert_cmpuint(psp_ccp_run(qts, &d), ==, 0);
    qtest_memread(qts, SRAM_DST, out, sizeof(out));
    g_assert_cmpmem(out, 256, rsa_c, 256);
    qtest_quit(qts);
}

/* 4096-bit: n = 2^4096 - 1, e = 3 in the LSB, m = 2^1000, so m^e = 2^3000 */
static void test_rsa_4096_lsb_key(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    uint8_t buf[1024], exp[512] = { 3 }, out[512], want[512] = { 0 };
    PspCcpDesc d;

    if (!qts) {
        return;
    }
    memset(buf, 0xff, 512);
    memset(buf + 512, 0, 512);
    buf[512 + 1000 / 8] = 1 << (1000 % 8);
    want[3000 / 8] = 1 << (3000 % 8);
    psp_rsa_desc(&d, 512, 0, CCP_MEMTYPE_SB);
    qtest_memwrite(qts, SRAM_SRC, buf, sizeof(buf));
    qtest_memwrite(qts, SRAM_DST, exp, sizeof(exp));
    g_assert_cmpuint(psp_ccp_run(qts, &(PspCcpDesc){
        .engine = CCP_ENGINE_PASSTHRU, .len = 512,
        .src = SRAM_DST, .src_mem = CCP_MEMTYPE_LOCAL,
        .dst = 0, .dst_mem = CCP_MEMTYPE_SB }), ==, 0);
    g_assert_cmpuint(psp_ccp_run(qts, &d), ==, 0);
    qtest_memread(qts, SRAM_DST, out, sizeof(out));
    g_assert_cmpmem(out, 512, want, 512);
    qtest_quit(qts);
}

/* Unsupported mode or size fails the descriptor instead of succeeding */
static void test_rsa_unsupported_fails(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);
    PspCcpDesc d;

    if (!qts) {
        return;
    }
    psp_rsa_desc(&d, 256, 1, CCP_MEMTYPE_LOCAL);
    g_assert_cmpuint(psp_ccp_run(qts, &d), !=, 0);
    psp_rsa_desc(&d, 600, 0, CCP_MEMTYPE_LOCAL);
    g_assert_cmpuint(psp_ccp_run(qts, &d), !=, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);

    qtest_add_func("/amd-psp/ccp/queue-registers", test_queue_registers);
    qtest_add_func("/amd-psp/ccp/passthrough", test_passthrough);
    qtest_add_func("/amd-psp/ccp/passthrough-lsb", test_passthrough_lsb);
    qtest_add_func("/amd-psp/ccp/sha256", test_sha256);
    qtest_add_func("/amd-psp/ccp/sha256-split", test_sha256_split);
    qtest_add_func("/amd-psp/ccp/sha256-init-restarts",
                   test_sha256_init_restarts);
    qtest_add_func("/amd-psp/ccp/sha384", test_sha384);
    qtest_add_func("/amd-psp/ccp/rsa-2048", test_rsa_2048);
    qtest_add_func("/amd-psp/ccp/rsa-4096-lsb-key", test_rsa_4096_lsb_key);
    qtest_add_func("/amd-psp/ccp/rsa-unsupported-fails",
                   test_rsa_unsupported_fails);
    qtest_add_func("/amd-psp/ccp/zlib", test_zlib);
    qtest_add_func("/amd-psp/ccp/zlib-error-leaves-device-usable",
                   test_zlib_error_leaves_device_usable);
    qtest_add_func("/amd-psp/ccp/trng-default-seed", test_trng_default_seed);
    qtest_add_func("/amd-psp/ccp/trng-seed", test_trng_seed);
    qtest_add_func("/amd-psp/ccp/trng-narrow-read", test_trng_narrow_read);

    return psp_test_run();
}
