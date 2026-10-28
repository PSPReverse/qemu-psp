/*
 * QTest for the Zen3 SMU message mailbox. Register layout and message ids
 * come from the emulator source, not from firmware.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "amd-psp-test.h"

/* SMN control (include/psp/smn/psp-smn.h): 16 bit slot n at +2n */
#define SMN_CTRL 0x03220000
#define SMN_WINDOW0 0x01000000          /* PSP address of slot 0 */
#define SMU_SMN_SLOT 0x3b               /* SMN 0x3b00000 */
#define SMU (SMN_WINDOW0 + 0x10700)     /* SMN 0x3b10700 */
#define SMU_ARG (SMU + 0x00)
#define SMU_RESP (SMU + 0x04)
#define SMU_MSG (SMU + 0x18)

#define MSG_QUERY_NUMBER_OF_DF_PSTATES 0x07
#define MSG_GET_STARTUP_DF_PSTATE 0x1d

static QTestState *start(const char *extra)
{
    QTestState *qts = psp_startf(PSP_ZEN3, "%s", extra);

    if (qts) {
        qtest_writew(qts, SMN_CTRL, SMU_SMN_SLOT);
    }
    return qts;
}

/* Send a message like the ABL: clear the response, set the argument */
static uint32_t smu_msg(QTestState *qts, uint32_t msg, uint32_t arg)
{
    qtest_writel(qts, SMU_RESP, 0);
    qtest_writel(qts, SMU_ARG, arg);
    qtest_writel(qts, SMU_MSG, msg);
    g_assert_cmphex(qtest_readl(qts, SMU_RESP), ==, 1);
    return qtest_readl(qts, SMU_ARG);
}

static void test_idle(void)
{
    QTestState *qts = start("");

    if (!qts) {
        return;
    }
    g_assert_cmphex(qtest_readl(qts, SMU_RESP), ==, 1);
    qtest_quit(qts);
}

static void test_df_pstates(void)
{
    QTestState *qts = start("");

    if (!qts) {
        return;
    }
    g_assert_cmphex(smu_msg(qts, MSG_QUERY_NUMBER_OF_DF_PSTATES, 0), ==, 1);
    qtest_quit(qts);
}

static void test_df_pstates_property(void)
{
    QTestState *qts = start("-global driver=amd_psp.smn.smu,"
                            "property=df-pstates,value=4");

    if (!qts) {
        return;
    }
    g_assert_cmphex(smu_msg(qts, MSG_QUERY_NUMBER_OF_DF_PSTATES, 0), ==, 4);
    qtest_quit(qts);
}

static void test_startup_pstate(void)
{
    QTestState *qts = start("");

    if (!qts) {
        return;
    }
    g_assert_cmphex(smu_msg(qts, MSG_GET_STARTUP_DF_PSTATE, 0x55), ==, 0);
    qtest_quit(qts);
}

static void test_unknown_message(void)
{
    QTestState *qts = start("");

    if (!qts) {
        return;
    }
    /* Answered OK with 0 whatever the argument, and the next one still works */
    g_assert_cmphex(smu_msg(qts, 0x42, 0x1234), ==, 0);
    g_assert_cmphex(smu_msg(qts, MSG_QUERY_NUMBER_OF_DF_PSTATES, 0), ==, 1);
    qtest_quit(qts);
}

/* The mailbox does not exist on Zen2: the argument does not turn into data */
static void test_zen2_absent(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    qtest_writew(qts, SMN_CTRL, SMU_SMN_SLOT);
    qtest_writel(qts, SMU_ARG, 0x1234);
    qtest_writel(qts, SMU_MSG, MSG_QUERY_NUMBER_OF_DF_PSTATES);
    g_assert_cmphex(qtest_readl(qts, SMU_ARG), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);

    qtest_add_func("/amd-psp/smu/idle", test_idle);
    qtest_add_func("/amd-psp/smu/df-pstates", test_df_pstates);
    qtest_add_func("/amd-psp/smu/df-pstates-property",
                   test_df_pstates_property);
    qtest_add_func("/amd-psp/smu/startup-pstate", test_startup_pstate);
    qtest_add_func("/amd-psp/smu/unknown-message", test_unknown_message);
    qtest_add_func("/amd-psp/smu/zen2-absent", test_zen2_absent);

    return psp_test_run();
}
