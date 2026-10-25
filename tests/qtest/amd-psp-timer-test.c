/*
 * QTest for the AMD PSP timers: register read-back, the counter, and the
 * opt-in expiry interrupt (-global driver=amd_psp.timer,
 * property=expiry_irq_id,value=N). Time only moves with qtest_clock_step().
 *
 * Expiry phase across a snapshot is a pending fix, see
 * amd-psp-pending-fixes-test.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "amd-psp-test.h"

#define EXPIRY_SRC 17
#define EXPIRY_ARGS "-global driver=amd_psp.timer,property=expiry_irq_id," \
                    "value=17"
#define HZ_ARGS "-global driver=amd_psp.timer,property=counter_hz," \
                "value=100000000"
/* A counter tick is 10 ns at 100 MHz, the expiry delay is period * 10 ns */
#define TICK_NS 10

static void timer_start(QTestState *qts, uint32_t period)
{
    qtest_writel(qts, TIMER1 + TIMER_PERIOD, period);
    qtest_writel(qts, TIMER1 + TIMER_CONTROL, 1);
}

static void assert_pending_only(QTestState *qts, int src)
{
    bool pending[IC_SOURCES];

    psp_ic_drain(qts, IC_BASE, pending);
    psp_ic_assert_only(pending, src);
}

/* Control, period, misc and placeholder words store what was written */
static void test_register_readback(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    qtest_writel(qts, TIMER1 + TIMER_PERIOD, 0x11223344);
    qtest_writel(qts, TIMER1 + TIMER_CONTROL, 0xf0);
    for (int i = 0; i < 3; i++) {
        qtest_writel(qts, TIMER1 + TIMER_MISC(i), 0x100 + i);
        qtest_writel(qts, TIMER1 + TIMER_PLACEHOLDER(i), 0x200 + i);
    }
    g_assert_cmphex(qtest_readl(qts, TIMER1 + TIMER_PERIOD), ==, 0x11223344);
    g_assert_cmphex(qtest_readl(qts, TIMER1 + TIMER_CONTROL), ==, 0xf0);
    for (int i = 0; i < 3; i++) {
        g_assert_cmphex(qtest_readl(qts, TIMER1 + TIMER_MISC(i)), ==,
                        0x100 + i);
        g_assert_cmphex(qtest_readl(qts, TIMER1 + TIMER_PLACEHOLDER(i)), ==,
                        0x200 + i);
    }
    /* Sub-word reads return the byte lanes of the word */
    g_assert_cmphex(qtest_readb(qts, TIMER1 + TIMER_PERIOD), ==, 0x44);
    g_assert_cmphex(qtest_readb(qts, TIMER1 + TIMER_PERIOD + 2), ==, 0x22);
    g_assert_cmphex(qtest_readw(qts, TIMER1 + TIMER_PERIOD + 2), ==, 0x1122);
    /* The two timers are independent */
    g_assert_cmphex(qtest_readl(qts, TIMER2 + TIMER_PERIOD), ==, 0);
    g_assert_cmphex(qtest_readl(qts, TIMER2 + TIMER_CONTROL), ==, 0);
    qtest_quit(qts);
}

/* Without counter_hz the counter is a software count: one per read */
static void test_counter_write(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    /* Disabled: it holds still */
    qtest_writel(qts, TIMER1 + TIMER_COUNT, 5);
    g_assert_cmpuint(qtest_readl(qts, TIMER1 + TIMER_COUNT), ==, 5);
    g_assert_cmpuint(qtest_readl(qts, TIMER1 + TIMER_COUNT), ==, 5);
    /* Enabled: each read returns the count and then advances it */
    qtest_writel(qts, TIMER1 + TIMER_CONTROL, 1);
    g_assert_cmpuint(qtest_readl(qts, TIMER1 + TIMER_COUNT), ==, 5);
    g_assert_cmpuint(qtest_readl(qts, TIMER1 + TIMER_COUNT), ==, 6);
    qtest_writel(qts, TIMER1 + TIMER_COUNT, 0xfffffffe);
    g_assert_cmpuint(qtest_readl(qts, TIMER1 + TIMER_COUNT), ==, 0xfffffffe);
    g_assert_cmpuint(qtest_readl(qts, TIMER1 + TIMER_COUNT), ==, 0xffffffff);
    g_assert_cmpuint(qtest_readl(qts, TIMER1 + TIMER_COUNT), ==, 0);
    /* The other timer did not move */
    g_assert_cmpuint(qtest_readl(qts, TIMER2 + TIMER_COUNT), ==, 0);
    qtest_quit(qts);
}

/* With counter_hz the counter follows the virtual clock from the last write */
static void test_counter_follows_clock(void)
{
    QTestState *qts = psp_startf(PSP_ZEN2, HZ_ARGS);

    if (!qts) {
        return;
    }
    qtest_writel(qts, TIMER1 + TIMER_COUNT, 1000);
    g_assert_cmpuint(qtest_readl(qts, TIMER1 + TIMER_COUNT), ==, 1000);
    /* Reads do not move it, enabled or not */
    qtest_writel(qts, TIMER1 + TIMER_CONTROL, 1);
    g_assert_cmpuint(qtest_readl(qts, TIMER1 + TIMER_COUNT), ==, 1000);
    qtest_clock_step(qts, 1000000);                     /* 1 ms */
    g_assert_cmpuint(qtest_readl(qts, TIMER1 + TIMER_COUNT), ==, 1000 + 100000);
    /* A write sets the counter and restarts the time base */
    qtest_writel(qts, TIMER1 + TIMER_COUNT, 7);
    g_assert_cmpuint(qtest_readl(qts, TIMER1 + TIMER_COUNT), ==, 7);
    qtest_clock_step(qts, 100);
    g_assert_cmpuint(qtest_readl(qts, TIMER1 + TIMER_COUNT), ==, 7 + 10);
    /* Timer 2 was never written: it counts from the machine reset, 1 ms ago */
    g_assert_cmpuint(qtest_readl(qts, TIMER2 + TIMER_COUNT), >=, 100000);
    qtest_quit(qts);
}

/* Enabled with a period, the timer raises its source every period */
static void test_expiry(void)
{
    QTestState *qts = psp_startf(PSP_ZEN2, EXPIRY_ARGS);
    const int64_t delay = 1000 * TICK_NS;

    if (!qts) {
        return;
    }
    timer_start(qts, 1000);
    qtest_clock_step(qts, delay - 1);
    assert_pending_only(qts, -1);
    qtest_clock_step(qts, 1);
    assert_pending_only(qts, EXPIRY_SRC);
    /* It re-arms itself: the next expiry is a full period later */
    qtest_clock_step(qts, delay - 1);
    assert_pending_only(qts, -1);
    qtest_clock_step(qts, 1);
    assert_pending_only(qts, EXPIRY_SRC);
    /* Several periods without an acknowledge are one pending source */
    qtest_clock_step(qts, 3 * delay);
    assert_pending_only(qts, EXPIRY_SRC);
    qtest_quit(qts);
}

/* Clearing the enable bit or the period stops it */
static void test_expiry_stop(void)
{
    QTestState *qts = psp_startf(PSP_ZEN2, EXPIRY_ARGS);

    if (!qts) {
        return;
    }
    timer_start(qts, 100);
    qtest_clock_step(qts, 100 * TICK_NS);
    assert_pending_only(qts, EXPIRY_SRC);
    qtest_writel(qts, TIMER1 + TIMER_CONTROL, 0);
    qtest_clock_step(qts, 100000);
    assert_pending_only(qts, -1);

    timer_start(qts, 100);
    qtest_writel(qts, TIMER1 + TIMER_PERIOD, 0);
    qtest_clock_step(qts, 100000);
    assert_pending_only(qts, -1);

    /* A period without the enable bit does not run either */
    qtest_writel(qts, TIMER1 + TIMER_CONTROL, 0);
    qtest_writel(qts, TIMER1 + TIMER_PERIOD, 100);
    qtest_clock_step(qts, 100000);
    assert_pending_only(qts, -1);
    qtest_quit(qts);
}

/* Very short periods are held to the 1 us minimum */
static void test_expiry_minimum_delay(void)
{
    QTestState *qts = psp_startf(PSP_ZEN2, EXPIRY_ARGS);

    if (!qts) {
        return;
    }
    timer_start(qts, 1);
    qtest_clock_step(qts, 999);
    assert_pending_only(qts, -1);
    qtest_clock_step(qts, 1);
    assert_pending_only(qts, EXPIRY_SRC);
    qtest_quit(qts);
}

/* The expiry interrupt is opt-in: without the property nothing is raised */
static void test_expiry_off_by_default(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    timer_start(qts, 100);
    qtest_clock_step(qts, 1000000);
    assert_pending_only(qts, -1);
    qtest_quit(qts);
}

/* Only timer 1 is wired to the controller */
static void test_timer2_has_no_interrupt(void)
{
    QTestState *qts = psp_startf(PSP_ZEN2, EXPIRY_ARGS);

    if (!qts) {
        return;
    }
    qtest_writel(qts, TIMER2 + TIMER_PERIOD, 100);
    qtest_writel(qts, TIMER2 + TIMER_CONTROL, 1);
    qtest_clock_step(qts, 1000000);
    assert_pending_only(qts, -1);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);

    qtest_add_func("/amd-psp/timer/register-readback", test_register_readback);
    qtest_add_func("/amd-psp/timer/counter-write", test_counter_write);
    qtest_add_func("/amd-psp/timer/counter-follows-clock",
                   test_counter_follows_clock);
    qtest_add_func("/amd-psp/timer/expiry", test_expiry);
    qtest_add_func("/amd-psp/timer/expiry-stop", test_expiry_stop);
    qtest_add_func("/amd-psp/timer/expiry-minimum-delay",
                   test_expiry_minimum_delay);
    qtest_add_func("/amd-psp/timer/expiry-off-by-default",
                   test_expiry_off_by_default);
    qtest_add_func("/amd-psp/timer/timer2-has-no-interrupt",
                   test_timer2_has_no_interrupt);

    return psp_test_run();
}
