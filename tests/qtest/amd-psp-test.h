/*
 * Shared helpers of the firmware-free AMD PSP qtests.
 *
 * Everything here is taken from the emulator source (register offsets,
 * descriptor layout, QOM paths); nothing comes from firmware. The machine is
 * started with an empty flash image generated at test time.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef TESTS_QTEST_AMD_PSP_TEST_H
#define TESTS_QTEST_AMD_PSP_TEST_H

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define PSP_ZEN "amd-psp-zen"
#define PSP_ZEN2 "amd-psp-zen2"
#define PSP_ZEN3 "amd-psp-zen3"

/* Mailbox (include/psp/psp.h, include/psp/mmio/mailbox.h) */
#define MBOX_WORD(n) (0x03010570 + 4 * (n))
#define MBOX_INT_ENABLE 0x0301069c
#define MBOX_INT_ENABLE_SECOS 0xff000000

/* Interrupt controller: Zen2 and later, and Zen/Zen+ (include/psp/psp.h) */
#define IC_BASE 0x03010200
#define IC_BASE_ZEN 0x03010300
#define IC_ENABLE(base, group) ((base) + 4 * (group))
#define IC_FIQ_HANDLED(base, group) ((base) + 0xb0 + 4 * (group))
#define IC_FIQ_ALL_PROCESSED(base) ((base) + 0xc0)
#define IC_FIQ_NEXT(base) ((base) + 0xc4)
#define IC_SOURCES 128
#define IC_PATH "/machine/soc/irq"

#define SRC_CCP 0x15
#define SRC_DOORBELL 0x3c

/* Timers */
#define TIMER1 0x03010400
#define TIMER2 0x03010424
#define TIMER_CONTROL 0x00
#define TIMER_MISC(n) (0x04 + 4 * (n))      /* n = 0..2 */
#define TIMER_PERIOD 0x10
#define TIMER_PLACEHOLDER(n) (0x14 + 4 * (n)) /* n = 0..2 */
#define TIMER_COUNT 0x20

/* CCP (include/psp/ccp/ccpv5.h, ccpv5-linux.h) */
#define CCP_BASE 0x03000000
#define CCP_TRNG (CCP_BASE + 0x0c)
#define CCP_Q0 (CCP_BASE + 0x1000)
#define CCP_Q_CTRL 0x0
#define CCP_Q_HEAD 0x4
#define CCP_Q_TAIL 0x8
#define CCP_Q_STATUS 0x100
#define CCP_Q_RUN 1
#define CCP_Q_HALT 2
#define CCP_INT_STATUS (CCP_BASE + 0x6008)
#define CCP_INT_STATUS_DONE (1u << 17)
#define CCP_ZLIB_WRITTEN (CCP_BASE + 0x6028)
#define CCP_ENGINE_SHA 3
#define CCP_ENGINE_RSA 4
#define CCP_ENGINE_PASSTHRU 5
#define CCP_ENGINE_ZLIB 6
#define CCP_ENGINE_ECC 7
#define CCP_MEMTYPE_SB 1
#define CCP_MEMTYPE_LOCAL 2
#define CCP_INIT (1u << 3)
#define CCP_EOM (1u << 4)
#define CCP_FUNC(f) ((f) << 5)
#define CCP_SHA256 CCP_FUNC(3u << 10)
#define CCP_SHA384 CCP_FUNC(4u << 10)
#define CCP_PT_BSWAP256 CCP_FUNC(2u)
#define CCP_DESC_SIZE 32
#define SRAM_DESC 0x1000
#define SRAM_SRC 0x2000
#define SRAM_DST 0x3000
#define SRAM_KEY 0x4000

/* x86 slot mapper (include/psp/x86/psp-x86-mapper.h) */
#define X86_CTRL 0x03230000
#define X86_SLOT_REGS 16                    /* four words per slot */
#define X86_WINDOW0 0x04000000              /* PSP address of slot 0 */
/* Slot base register for x86 address 0xfffdfc000000 (6 + 26 bit fields) */
#define X86_BASE_PROTECTED_MMIO ((0xfffdu << 6) | 0x3f)
/* I/O port 0x61 once slot 0 is mapped there */
#define PORT61 (X86_WINDOW0 + 0x61)

/* zlib stream of "abcd" in one stored block, with its Adler-32 */
static const uint8_t psp_zlib_abcd[] = {
    0x78, 0x01, 0x01, 0x04, 0x00, 0xfb, 0xff, 'a', 'b', 'c', 'd',
    0x03, 0xd8, 0x01, 0x8b,
};

typedef struct PspCcpDesc {
    unsigned engine;
    uint32_t flags;         /* CCP_INIT, CCP_EOM, CCP_FUNC() */
    uint32_t len;
    uint32_t src;
    unsigned src_mem;
    uint32_t dst;           /* SHA: length low */
    unsigned dst_mem;
    unsigned lsb;           /* LSB context slot */
    uint32_t key;           /* RSA: exponent address */
    unsigned key_mem;
} PspCcpDesc;

static char *psp_flash_path;
static const char *psp_skip_reason;

/* Common start of main(): one machine check, one empty flash image */
static inline void psp_test_init(int *argc, char ***argv)
{
    g_autoptr(GError) err = NULL;
    static const uint8_t flash[4096];
    int fd;

    g_test_init(argc, argv, NULL);

    if (!qtest_has_machine(PSP_ZEN2) || !qtest_has_machine(PSP_ZEN3) ||
        !qtest_has_machine(PSP_ZEN)) {
        psp_skip_reason = "AMD PSP machines not available";
        return;
    }
    /* The machine needs a flash image; an empty one is enough here */
    fd = g_file_open_tmp("psp-flash-XXXXXX", &psp_flash_path, &err);
    if (fd < 0 || write(fd, flash, sizeof(flash)) != sizeof(flash)) {
        psp_skip_reason = "cannot create a flash image";
    }
    if (fd >= 0) {
        close(fd);
    }
}

static inline int psp_test_run(void)
{
    int ret = g_test_run();

    if (psp_flash_path) {
        unlink(psp_flash_path);
        g_free(psp_flash_path);
    }
    return ret;
}

/* NULL, with the test marked skipped, if the machine cannot be started */
static inline QTestState *psp_startf(const char *machine, const char *fmt, ...)
    G_GNUC_PRINTF(2, 3);
static inline QTestState *psp_startf(const char *machine, const char *fmt, ...)
{
    g_autofree char *extra = NULL;
    va_list ap;

    if (psp_skip_reason) {
        g_test_skip(psp_skip_reason);
        return NULL;
    }
    va_start(ap, fmt);
    extra = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    return qtest_initf("-machine %s "
                       "-global driver=amd_psp.smnflash,property=flash_img,"
                       "value=%s %s",
                       machine, psp_flash_path, extra);
}

#define psp_start(machine) psp_startf((machine), "%s", "")

/* Machine-wide */

static inline void psp_system_reset(QTestState *qts)
{
    qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    qtest_qmp_eventwait(qts, "RESET");
}

/* Raise IC source id through the controller's QOM hook, as a device would */
static inline void psp_ic_inject(QTestState *qts, unsigned id)
{
    qtest_qmp_assert_success(qts,
                             "{'execute': 'qom-set', 'arguments': "
                             "{'path': %s, 'property': 'inject', "
                             "'value': %u}}", IC_PATH, id);
}

/* The controller only reports pending sources the firmware has enabled */
static inline void psp_ic_enable_all(QTestState *qts, uint32_t base)
{
    for (int g = 0; g < IC_SOURCES / 32; g++) {
        qtest_writel(qts, IC_ENABLE(base, g), UINT32_MAX);
    }
}

/*
 * Collect the pending sources by acknowledging them one by one: the
 * controller only reports the lowest pending source. Enables all sources
 * first, as the firmware does.
 */
static inline void psp_ic_drain(QTestState *qts, uint32_t base,
                                bool pending[IC_SOURCES])
{
    memset(pending, 0, IC_SOURCES * sizeof(bool));
    psp_ic_enable_all(qts, base);
    while (!qtest_readl(qts, IC_FIQ_ALL_PROCESSED(base))) {
        uint32_t src = qtest_readl(qts, IC_FIQ_NEXT(base));

        g_assert_cmpuint(src, <, IC_SOURCES);
        g_assert_false(pending[src]);
        pending[src] = true;
        qtest_writel(qts, IC_FIQ_HANDLED(base, src / 32), 1u << (src % 32));
    }
}

/* Exactly source src is pending; -1: none */
static inline void psp_ic_assert_only(const bool pending[IC_SOURCES], int src)
{
    for (int i = 0; i < IC_SOURCES; i++) {
        g_assert_cmpint(pending[i], ==, i == src);
    }
}

/* CCP */

/* Run one descriptor on queue 0 and return the queue status */
static inline uint32_t psp_ccp_run(QTestState *qts, const PspCcpDesc *d)
{
    uint32_t w[CCP_DESC_SIZE / 4] = {
        (d->engine << 20) | d->flags,
        d->len,
        d->src,
        (d->src_mem << 16) | (d->lsb << 18),
        d->dst,
        d->dst_mem << 16,
        d->key,
        d->key_mem << 16,
    };

    for (int i = 0; i < ARRAY_SIZE(w); i++) {
        qtest_writel(qts, SRAM_DESC + 4 * i, w[i]);
    }
    qtest_writel(qts, CCP_Q0 + CCP_Q_TAIL, SRAM_DESC);
    qtest_writel(qts, CCP_Q0 + CCP_Q_HEAD, SRAM_DESC + CCP_DESC_SIZE);
    qtest_writel(qts, CCP_Q0 + CCP_Q_CTRL, CCP_Q_RUN);
    /* The queue runs when the control register is read with RUN set */
    qtest_readl(qts, CCP_Q0 + CCP_Q_CTRL);
    return qtest_readl(qts, CCP_Q0 + CCP_Q_STATUS);
}

/* Inflate len bytes of zlib at SRAM_SRC to SRAM_DST */
static inline uint32_t psp_ccp_zlib(QTestState *qts, uint32_t len)
{
    PspCcpDesc d = {
        .engine = CCP_ENGINE_ZLIB, .flags = CCP_INIT | CCP_EOM, .len = len,
        .src = SRAM_SRC, .src_mem = CCP_MEMTYPE_LOCAL,
        .dst = SRAM_DST, .dst_mem = CCP_MEMTYPE_LOCAL,
    };

    return psp_ccp_run(qts, &d);
}

/* Copy len bytes from SRAM src to SRAM dst through the CCP */
static inline uint32_t psp_ccp_copy(QTestState *qts, uint32_t dst,
                                    uint32_t src, uint32_t len)
{
    PspCcpDesc d = {
        .engine = CCP_ENGINE_PASSTHRU, .len = len,
        .src = src, .src_mem = CCP_MEMTYPE_LOCAL,
        .dst = dst, .dst_mem = CCP_MEMTYPE_LOCAL,
    };

    return psp_ccp_run(qts, &d);
}

/*
 * SHA-256 over SRAM [src, src + len). The digest ends up in LSB slot lsb,
 * byte-reversed as the CCP stores it.
 */
static inline uint32_t psp_ccp_sha256(QTestState *qts, uint32_t src,
                                      uint32_t len, unsigned lsb,
                                      uint32_t flags)
{
    PspCcpDesc d = {
        .engine = CCP_ENGINE_SHA, .flags = CCP_SHA256 | flags, .len = len,
        .src = src, .src_mem = CCP_MEMTYPE_LOCAL, .dst = 0x20, .lsb = lsb,
    };

    return psp_ccp_run(qts, &d);
}

/*
 * Read the 32-byte digest in LSB slot lsb, byte-swapped back to the natural
 * order by a 256-bit swapping passthrough, into out.
 */
static inline void psp_ccp_read_digest(QTestState *qts, unsigned lsb,
                                       uint8_t out[32])
{
    PspCcpDesc d = {
        .engine = CCP_ENGINE_PASSTHRU, .flags = CCP_PT_BSWAP256, .len = 32,
        .src = lsb * 32, .src_mem = CCP_MEMTYPE_SB,
        .dst = SRAM_DST, .dst_mem = CCP_MEMTYPE_LOCAL,
    };

    g_assert_cmpuint(psp_ccp_run(qts, &d), ==, 0);
    qtest_memread(qts, SRAM_DST, out, 32);
}

static inline void psp_expected_sha256(const void *data, size_t len,
                                       uint8_t out[32])
{
    gsize outlen = 32;
    GChecksum *c = g_checksum_new(G_CHECKSUM_SHA256);

    g_checksum_update(c, data, len);
    g_checksum_get_digest(c, out, &outlen);
    g_assert_cmpuint(outlen, ==, 32);
    g_checksum_free(c);
}

/* Migration through a file: save the state of src, load it into a new one */

static inline void psp_wait_migration(QTestState *qts)
{
    for (int i = 0; i < 300; i++) {
        QDict *rsp = qtest_qmp(qts, "{'execute': 'query-migrate'}");
        QDict *ret = qdict_get_qdict(rsp, "return");
        const char *status = ret ? qdict_get_try_str(ret, "status") : NULL;
        bool done = status && !strcmp(status, "completed");

        g_assert_cmpstr(status, !=, "failed");
        qobject_unref(rsp);
        if (done) {
            return;
        }
        g_usleep(100 * 1000);
    }
    g_test_fail();
    g_assert_not_reached();
}

/*
 * Start a second machine with the same command line plus extra, and load
 * the state of src into it. src is left alone, still usable for reads.
 */
static inline QTestState *psp_migrate_clone(QTestState *src,
                                            const char *machine,
                                            const char *extra)
{
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    QTestState *dst;
    int fd = g_file_open_tmp("psp-state-XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    uri = g_strdup_printf("file:%s", path);
    qtest_qmp_assert_success(src,
                             "{'execute': 'migrate', 'arguments': "
                             "{'uri': %s}}", uri);
    psp_wait_migration(src);
    dst = psp_startf(machine, "-incoming defer %s", extra);
    g_assert_nonnull(dst);
    qtest_qmp_assert_success(dst,
                             "{'execute': 'migrate-incoming', 'arguments': "
                             "{'uri': %s}}", uri);
    psp_wait_migration(dst);
    unlink(path);
    return dst;
}

#endif
