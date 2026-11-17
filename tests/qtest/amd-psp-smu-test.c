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
#define MSG_QUERY_MEM_FREQ_OF_DF_PSTATE 0x08
#define MSG_GET_STARTUP_DF_PSTATE 0x1d
#define MSG_QUERY_CCD_MASK 0x10
#define MSG_QUERY_CCX_MASK 0x12
#define MSG_READ_X86_MSR 0x2b
#define MSR_SYSCFG 0xc0010010

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

static void test_mem_freq(void)
{
    QTestState *qts = start("");

    if (!qts) {
        return;
    }
    g_assert_cmphex(smu_msg(qts, MSG_QUERY_MEM_FREQ_OF_DF_PSTATE, 0), ==,
                    1200);
    g_assert_cmphex(smu_msg(qts, MSG_QUERY_MEM_FREQ_OF_DF_PSTATE, 1), ==,
                    1200);
    qtest_quit(qts);
}

static void test_mem_freq_property(void)
{
    QTestState *qts = start("-global driver=amd_psp.smn.smu,"
                            "property=mem-clk-mhz,value=1600");

    if (!qts) {
        return;
    }
    g_assert_cmphex(smu_msg(qts, MSG_QUERY_MEM_FREQ_OF_DF_PSTATE, 0), ==,
                    1600);
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

/* One CCD by default; the CCX map answers per CCD index */
static void test_core_maps(void)
{
    QTestState *qts = start("");

    if (!qts) {
        return;
    }
    g_assert_cmphex(smu_msg(qts, MSG_QUERY_CCD_MASK, 0), ==, 0x1);
    g_assert_cmphex(smu_msg(qts, MSG_QUERY_CCX_MASK, 0), ==, 0x1);
    g_assert_cmphex(smu_msg(qts, MSG_QUERY_CCX_MASK, 1), ==, 0);
    qtest_quit(qts);
}

static void test_core_maps_property(void)
{
    QTestState *qts = start("-global driver=amd_psp.smn.smu,"
                            "property=ccd-mask,value=0x5");

    if (!qts) {
        return;
    }
    g_assert_cmphex(smu_msg(qts, MSG_QUERY_CCD_MASK, 0), ==, 0x5);
    g_assert_cmphex(smu_msg(qts, MSG_QUERY_CCX_MASK, 2), ==, 0x1);
    g_assert_cmphex(smu_msg(qts, MSG_QUERY_CCX_MASK, 1), ==, 0);
    qtest_quit(qts);
}

/* MSR read: out0 = 1 in ARG, value low in +8, high in +0xc */
static uint32_t msr_read(QTestState *qts, uint32_t msr, uint32_t *hi)
{
    g_assert_cmphex(smu_msg(qts, MSG_READ_X86_MSR, msr), ==, 1);
    *hi = qtest_readl(qts, SMU_ARG + 0xc);
    return qtest_readl(qts, SMU_ARG + 0x8);
}

/* x86 MSR read: SYSCFG has SNPEn (bit 24) set by default, others read 0 */
static void test_msr_syscfg(void)
{
    QTestState *qts = start("");

    if (!qts) {
        return;
    }
    uint32_t hi;

    g_assert_cmphex(msr_read(qts, MSR_SYSCFG, &hi), ==, 0x03fc0000);
    g_assert_cmphex(hi, ==, 0);
    g_assert_cmphex(msr_read(qts, 0xc0010015, &hi), ==, 1);
    g_assert_cmphex(msr_read(qts, 0xc0011006, &hi), ==, 0);
    qtest_quit(qts);
}

/* SMM_ADDR / SMM_MASK are 64-bit and read 0 unless set */
static void test_msr_smm(void)
{
    QTestState *qts = start("-global driver=amd_psp.smn.smu,"
                            "property=smm-addr,value=0x2000000 "
                            "-global driver=amd_psp.smn.smu,"
                            "property=smm-mask,value=0xffffffffffc00000");
    uint32_t hi;

    if (!qts) {
        return;
    }
    g_assert_cmphex(msr_read(qts, 0xc0010112, &hi), ==, 0x2000000);
    g_assert_cmphex(hi, ==, 0);
    g_assert_cmphex(msr_read(qts, 0xc0010113, &hi), ==, 0xffc00000);
    g_assert_cmphex(hi, ==, 0xffffffff);
    qtest_quit(qts);
}

/* GetSmuVersion (message 2) follows the smu-version property */
static void test_smu_version(void)
{
    QTestState *qts = start("");

    if (!qts) {
        return;
    }
    g_assert_cmphex(smu_msg(qts, 0x02, 0), ==, 0);
    qtest_quit(qts);
    qts = start("-global driver=amd_psp.smn.smu,"
                "property=smu-version,value=0x2d6200");
    if (!qts) {
        return;
    }
    g_assert_cmphex(smu_msg(qts, 0x02, 0), ==, 0x2d6200);
    qtest_quit(qts);
}

/* TOM / TOM2 are 64-bit and read 0 unless set */
static void test_msr_tom(void)
{
    QTestState *qts = start("-global driver=amd_psp.smn.smu,"
                            "property=tom,value=0x80000000 "
                            "-global driver=amd_psp.smn.smu,"
                            "property=tom2,value=0x250000000");
    uint32_t hi;

    if (!qts) {
        return;
    }
    g_assert_cmphex(msr_read(qts, 0xc001001a, &hi), ==, 0x80000000);
    g_assert_cmphex(hi, ==, 0);
    g_assert_cmphex(msr_read(qts, 0xc001001d, &hi), ==, 0x50000000);
    g_assert_cmphex(hi, ==, 2);
    qtest_quit(qts);
}

static void test_msr_syscfg_property(void)
{
    QTestState *qts = start("-global driver=amd_psp.smn.smu,"
                            "property=syscfg,value=0x800000");

    if (!qts) {
        return;
    }
    uint32_t hi;

    g_assert_cmphex(msr_read(qts, MSR_SYSCFG, &hi), ==, 0x800000);
    qtest_quit(qts);
}

/* Zen2 has the same mailbox: idle response 1, any message answers 1 */
static void test_zen2_present(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    qtest_writew(qts, SMN_CTRL, SMU_SMN_SLOT);
    g_assert_cmphex(qtest_readl(qts, SMU_RESP), ==, 1);
    qtest_writel(qts, SMU_RESP, 0);
    qtest_writel(qts, SMU_MSG, 0x19);
    g_assert_cmphex(qtest_readl(qts, SMU_RESP), ==, 1);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);

    qtest_add_func("/amd-psp/smu/idle", test_idle);
    qtest_add_func("/amd-psp/smu/df-pstates", test_df_pstates);
    qtest_add_func("/amd-psp/smu/df-pstates-property",
                   test_df_pstates_property);
    qtest_add_func("/amd-psp/smu/mem-freq", test_mem_freq);
    qtest_add_func("/amd-psp/smu/mem-freq-property",
                   test_mem_freq_property);
    qtest_add_func("/amd-psp/smu/startup-pstate", test_startup_pstate);
    qtest_add_func("/amd-psp/smu/unknown-message", test_unknown_message);
    qtest_add_func("/amd-psp/smu/core-maps", test_core_maps);
    qtest_add_func("/amd-psp/smu/core-maps-property", test_core_maps_property);
    qtest_add_func("/amd-psp/smu/msr-syscfg", test_msr_syscfg);
    qtest_add_func("/amd-psp/smu/msr-smm", test_msr_smm);
    qtest_add_func("/amd-psp/smu/msr-tom", test_msr_tom);
    qtest_add_func("/amd-psp/smu/smu-version", test_smu_version);
    qtest_add_func("/amd-psp/smu/msr-syscfg-property",
                   test_msr_syscfg_property);
    qtest_add_func("/amd-psp/smu/zen2-present", test_zen2_present);

    return psp_test_run();
}
