/*
 * QTest for PSP_TODO, the marker for emulator gaps. The exported
 * aspfuzz_todo_* functions (per-site hit counters and bitmap) live in the
 * QEMU process and cannot be reached from qtest, so hits are observed in the
 * "-d unimp" log, one line per hit. The only site reachable from a plain
 * machine is ccp.engine.unknown: a CCP descriptor with an engine number the
 * model does not know.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "amd-psp-test.h"

#define SITE "PSP_TODO ccp.engine.unknown"
#define CANARY_LEN 16

/* What a guest can see after the hits */
typedef struct Visible {
    uint32_t status[4];
    uint32_t ctrl, head, tail, int_status;
    uint8_t canary[CANARY_LEN];
} Visible;

static unsigned count_hits(const char *log)
{
    unsigned n = 0;

    for (const char *p = strstr(log, SITE); p; p = strstr(p + 1, SITE)) {
        n++;
    }
    return n;
}

/*
 * Run n descriptors with unknown engine numbers and return the log. extra
 * is appended to the command line, e.g. an -object psp-todo.
 */
static char *run_unknown_engines(const char *extra, unsigned n, Visible *v)
{
    g_autofree char *path = NULL;
    char *log = NULL;
    QTestState *qts;
    int fd = g_file_open_tmp("psp-log-XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_assert_cmpuint(n, <=, ARRAY_SIZE(v->status));

    memset(v, 0, sizeof(*v));
    qts = psp_startf(PSP_ZEN2, "%s -d unimp -D %s", extra, path);
    g_assert_nonnull(qts);
    memset(v->canary, 0x5a, CANARY_LEN);
    qtest_memwrite(qts, SRAM_DST, v->canary, CANARY_LEN);
    for (unsigned i = 0; i < n; i++) {
        PspCcpDesc d = {
            .engine = 8 + i,    /* 0..7 are known engines */
            .len = CANARY_LEN,
            .src = SRAM_SRC, .src_mem = CCP_MEMTYPE_LOCAL,
            .dst = SRAM_DST, .dst_mem = CCP_MEMTYPE_LOCAL,
        };

        v->status[i] = psp_ccp_run(qts, &d);
    }
    v->ctrl = qtest_readl(qts, CCP_Q0 + CCP_Q_CTRL);
    v->head = qtest_readl(qts, CCP_Q0 + CCP_Q_HEAD);
    v->tail = qtest_readl(qts, CCP_Q0 + CCP_Q_TAIL);
    v->int_status = qtest_readl(qts, CCP_INT_STATUS);
    qtest_memread(qts, SRAM_DST, v->canary, CANARY_LEN);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(path, &log, NULL, NULL));
    unlink(path);
    return log;
}

/* Without a psp-todo object the policy is log: every hit is one line */
static void test_default_logs_every_hit(void)
{
    g_autofree char *log = NULL;
    Visible v;

    if (psp_skip_reason) {
        g_test_skip(psp_skip_reason);
        return;
    }
    log = run_unknown_engines("", 3, &v);
    g_assert_cmpuint(count_hits(log), ==, 3);
    /* The line says what the guest sees */
    g_assert_nonnull(strstr(log, "guest sees: descriptor skipped"));
    g_assert_nonnull(strstr(log, "val 0x8"));
    g_assert_nonnull(strstr(log, "val 0xa"));
}

static void test_policy_log(void)
{
    g_autofree char *log = NULL;
    Visible v;

    if (psp_skip_reason) {
        g_test_skip(psp_skip_reason);
        return;
    }
    log = run_unknown_engines("-object psp-todo,id=t,policy=log", 2, &v);
    g_assert_cmpuint(count_hits(log), ==, 2);
}

static void test_policy_ignore(void)
{
    g_autofree char *log = NULL;
    Visible v;

    if (psp_skip_reason) {
        g_test_skip(psp_skip_reason);
        return;
    }
    log = run_unknown_engines("-object psp-todo,id=t,policy=ignore", 3, &v);
    g_assert_cmpuint(count_hits(log), ==, 0);
}

/*
 * The policy only decides what the host does with a hit; the guest sees the
 * same thing under ignore and log: success, no data written, same registers.
 */
static void test_policy_has_no_guest_visible_effect(void)
{
    g_autofree char *log_ignore = NULL;
    g_autofree char *log_log = NULL;
    Visible ignore, logged;

    if (psp_skip_reason) {
        g_test_skip(psp_skip_reason);
        return;
    }
    log_ignore = run_unknown_engines("-object psp-todo,id=t,policy=ignore", 3,
                                     &ignore);
    log_log = run_unknown_engines("-object psp-todo,id=t,policy=log", 3,
                                  &logged);
    g_assert_cmpuint(count_hits(log_ignore), ==, 0);
    g_assert_cmpuint(count_hits(log_log), ==, 3);
    g_assert_cmpmem(&ignore, sizeof(ignore), &logged, sizeof(logged));
    /* Reported as successful, and the destination was not touched */
    for (int i = 0; i < 3; i++) {
        g_assert_cmpuint(ignore.status[i], ==, 0);
    }
    for (int i = 0; i < CANARY_LEN; i++) {
        g_assert_cmphex(ignore.canary[i], ==, 0x5a);
    }
}

/* The policy object can be changed while the machine runs */
static void test_policy_at_runtime(void)
{
    g_autofree char *path = NULL;
    g_autofree char *log = NULL;
    QTestState *qts;
    const char *policies[] = {"log", "ignore", "ignore", "log"};
    int fd;

    if (psp_skip_reason) {
        g_test_skip(psp_skip_reason);
        return;
    }
    fd = g_file_open_tmp("psp-log-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    qts = psp_startf(PSP_ZEN2, "-object psp-todo,id=t,policy=log "
                     "-d unimp -D %s", path);
    for (int i = 0; i < ARRAY_SIZE(policies); i++) {
        PspCcpDesc d = {.engine = 15, .len = 4, .src_mem = CCP_MEMTYPE_LOCAL,
                        .dst_mem = CCP_MEMTYPE_LOCAL};

        qtest_qmp_assert_success(qts,
                                 "{'execute': 'qom-set', 'arguments': "
                                 "{'path': '/objects/t', 'property': 'policy', "
                                 "'value': %s}}", policies[i]);
        psp_ccp_run(qts, &d);
    }
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(path, &log, NULL, NULL));
    unlink(path);
    g_assert_cmpuint(count_hits(log), ==, 2);
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);

    qtest_add_func("/amd-psp/todo/default-logs-every-hit",
                   test_default_logs_every_hit);
    qtest_add_func("/amd-psp/todo/policy-log", test_policy_log);
    qtest_add_func("/amd-psp/todo/policy-ignore", test_policy_ignore);
    qtest_add_func("/amd-psp/todo/policy-no-guest-visible-effect",
                   test_policy_has_no_guest_visible_effect);
    qtest_add_func("/amd-psp/todo/policy-at-runtime", test_policy_at_runtime);

    return psp_test_run();
}
