/*
 * QTest for the Zen3 UMC channel APB windows and the PMU mailbox behind them.
 * Register layout comes from the emulator source, not from firmware.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "amd-psp-test.h"

/* SMN control (include/psp/smn/psp-smn.h): 16 bit slot n at +2n */
#define SMN_CTRL 0x03220000
#define SMN_WINDOW0 0x01000000          /* PSP address of slot 0 */
/* Channel c sits in SMN 1 MiB slot c at offset 0x51800 */
#define UMC_ADDR 0x51800
#define UMC_DATA 0x51804
#define UMC_STATUS 0x51808

#define APB_PENDING 0xd0004
#define APB_ACK 0xd0031
#define APB_CODE 0xd0032
#define APB_STREAM 0xd0034
#define APB_MSGBLK 0x54000
#define WRITE_FLAG 0x80000000u

static QTestState *start(const char *extra)
{
    return psp_startf(PSP_ZEN3, "%s", extra);
}

/* Point SMN window 0 at the SMN slot of the channel */
static void select_chan(QTestState *qts, unsigned chan)
{
    qtest_writew(qts, SMN_CTRL, chan);
}

static uint32_t reg(unsigned off)
{
    return SMN_WINDOW0 + off;
}

/* Read APB register like the ABL: address, poll status, data */
static uint32_t apb_read(QTestState *qts, unsigned chan, uint32_t apb)
{
    select_chan(qts, chan);
    qtest_writel(qts, reg(UMC_ADDR), apb);
    g_assert_cmphex(qtest_readl(qts, reg(UMC_STATUS)) & 1, ==, 1);
    return qtest_readl(qts, reg(UMC_DATA));
}

static void apb_write(QTestState *qts, unsigned chan, uint32_t apb,
                      uint32_t val)
{
    select_chan(qts, chan);
    qtest_writel(qts, reg(UMC_DATA), val);
    qtest_writel(qts, reg(UMC_ADDR), apb | WRITE_FLAG);
    g_assert_cmphex(qtest_readl(qts, reg(UMC_STATUS)) & 1, ==, 1);
}

static void test_status_ready(void)
{
    QTestState *qts = start("");

    if (!qts) {
        return;
    }
    for (unsigned c = 0; c < 8; c++) {
        select_chan(qts, c);
        g_assert_cmphex(qtest_readl(qts, reg(UMC_STATUS)) & 1, ==, 1);
    }
    /* Also after an address write, and the registers read back */
    select_chan(qts, 6);
    qtest_writel(qts, reg(UMC_ADDR), 0x123);
    g_assert_cmphex(qtest_readl(qts, reg(UMC_STATUS)) & 1, ==, 1);
    g_assert_cmphex(qtest_readl(qts, reg(UMC_ADDR)), ==, 0x123);
    qtest_quit(qts);
}

static void test_kept_ranges(void)
{
    QTestState *qts = start("");

    if (!qts) {
        return;
    }
    apb_write(qts, 2, APB_MSGBLK + 4 * 3, 0xcafe);
    apb_write(qts, 2, APB_STREAM, 0x1234);
    g_assert_cmphex(apb_read(qts, 2, APB_MSGBLK + 4 * 3), ==, 0xcafe);
    g_assert_cmphex(apb_read(qts, 2, APB_STREAM), ==, 0x1234);
    g_assert_cmphex(apb_read(qts, 2, APB_MSGBLK), ==, 0);
    /* The write flag is not part of the address */
    g_assert_cmphex(apb_read(qts, 2, APB_MSGBLK + 4 * 3), ==, 0xcafe);
    qtest_quit(qts);
}

static void test_unknown_ignored(void)
{
    QTestState *qts = start("");

    if (!qts) {
        return;
    }
    apb_write(qts, 0, 0x40000, 0xdead);
    g_assert_cmphex(apb_read(qts, 0, 0x40000), ==, 0);
    /* The 20 bit address wraps nothing into the kept ranges */
    g_assert_cmphex(apb_read(qts, 0, APB_STREAM), ==, 0);
    qtest_quit(qts);
}

static void test_channel_isolation(void)
{
    QTestState *qts = start("");

    if (!qts) {
        return;
    }
    apb_write(qts, 0, APB_MSGBLK, 0x11);
    apb_write(qts, 7, APB_MSGBLK, 0x77);
    g_assert_cmphex(apb_read(qts, 0, APB_MSGBLK), ==, 0x11);
    g_assert_cmphex(apb_read(qts, 7, APB_MSGBLK), ==, 0x77);
    g_assert_cmphex(apb_read(qts, 3, APB_MSGBLK), ==, 0);
    /* Handshake state is per channel too */
    apb_write(qts, 1, APB_ACK, 0);
    g_assert_cmphex(apb_read(qts, 1, APB_PENDING), ==, 1);
    g_assert_cmphex(apb_read(qts, 4, APB_PENDING), ==, 0);
    qtest_quit(qts);
}

static void mailbox_sequence(QTestState *qts, unsigned chan, uint32_t code)
{
    /* Message available, read it, ack, wait for the clear, release */
    g_assert_cmphex(apb_read(qts, chan, APB_PENDING) & 1, ==, 0);
    g_assert_cmphex(apb_read(qts, chan, APB_CODE), ==, code);
    apb_write(qts, chan, APB_ACK, 0);
    g_assert_cmphex(apb_read(qts, chan, APB_PENDING) & 1, ==, 1);
    apb_write(qts, chan, APB_ACK, 1);
}

static void test_mailbox(void)
{
    QTestState *qts = start("");

    if (!qts) {
        return;
    }
    /* A second message is available after the first was released */
    mailbox_sequence(qts, 6, 7);
    mailbox_sequence(qts, 6, 7);
    mailbox_sequence(qts, 0, 7);
    qtest_quit(qts);
}

static void test_pmu_message_property(void)
{
    QTestState *qts = start("-global driver=amd_psp.smn.umc-apb,"
                            "property=pmu-message,value=0xff");

    if (!qts) {
        return;
    }
    mailbox_sequence(qts, 5, 0xff);
    qtest_quit(qts);
}

/* The windows do not exist on Zen2: no ready status, no data */
static void test_zen2_absent(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    select_chan(qts, 0);
    qtest_writel(qts, reg(UMC_DATA), 0x55);
    qtest_writel(qts, reg(UMC_ADDR), APB_MSGBLK | WRITE_FLAG);
    g_assert_cmphex(qtest_readl(qts, reg(UMC_STATUS)) & 1, ==, 0);
    g_assert_cmphex(qtest_readl(qts, reg(UMC_DATA)), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);

    qtest_add_func("/amd-psp/umc/status-ready", test_status_ready);
    qtest_add_func("/amd-psp/umc/kept-ranges", test_kept_ranges);
    qtest_add_func("/amd-psp/umc/unknown-ignored", test_unknown_ignored);
    qtest_add_func("/amd-psp/umc/channel-isolation", test_channel_isolation);
    qtest_add_func("/amd-psp/umc/mailbox", test_mailbox);
    qtest_add_func("/amd-psp/umc/pmu-message-property",
                   test_pmu_message_property);
    qtest_add_func("/amd-psp/umc/zen2-absent", test_zen2_absent);

    return psp_test_run();
}
