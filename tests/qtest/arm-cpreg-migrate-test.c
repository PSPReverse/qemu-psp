/*
 * QTest for migration of banked AArch32 coprocessor registers.
 *
 * A guest running under TCG in Secure state writes DACR (banked, so the
 * Secure and Non-secure copies are separate) and copies it to RAM in a
 * loop. The machine is saved with "migrate", restored in a second machine
 * and the value the restored guest reads back is compared. Needs a real
 * TCG guest, so the machines are spawned directly rather than through
 * the qtest accelerator.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "qemu/sockets.h"
#include "libqtest.h"
#include "libqmp.h"
#include "qobject/qdict.h"

#define RAM_BASE   0x60000000
#define OUT_ADDR   (RAM_BASE + 0x1000)
#define DACR_VAL   0x55555555

/*
 * mov r0, #0x55; orr r0, r0, r0, lsl #8; orr r0, r0, r0, lsl #16
 * mcr p15, 0, r0, c3, c0, 0   (DACR)
 * mov r1, #RAM_BASE; add r1, r1, #0x1000
 * loop: mrc p15, 0, r2, c3, c0, 0; str r2, [r1]; b loop
 */
static const uint32_t guest[] = {
    0xe3a00055, 0xe1800400, 0xe1800800, 0xee030f10, 0xe3a01206,
    0xe2811a01, 0xee132f10, 0xe5812000, 0xeafffffc,
};

typedef struct {
    GPid pid;
    int fd;
} Vm;

/* Like qmp_fd(), but skip events */
static QDict *vm_qmp(Vm *vm, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static QDict *vm_qmp(Vm *vm, const char *fmt, ...)
{
    va_list ap;
    QDict *rsp;

    va_start(ap, fmt);
    qmp_fd_vsend(vm->fd, fmt, ap);
    va_end(ap);
    do {
        rsp = qmp_fd_receive(vm->fd);
        if (!qdict_haskey(rsp, "event")) {
            return rsp;
        }
        qobject_unref(rsp);
    } while (1);
}

static Vm vm_start(const char *dir, const char *tag, const char *extra)
{
    const char *bin = getenv("QTEST_QEMU_BINARY");
    g_autofree char *sock = g_strdup_printf("%s/%s.sock", dir, tag);
    g_autofree char *cmd = g_strdup_printf(
        "%s -machine vexpress-a9 -accel tcg -display none -monitor none "
        "-serial none -qmp unix:%s,server=on,wait=off %s",
        bin, sock, extra);
    g_auto(GStrv) argv = NULL;
    g_autoptr(QDict) greeting = NULL;
    Vm vm;

    g_assert_nonnull(bin);
    g_assert_true(g_shell_parse_argv(cmd, NULL, &argv, NULL));
    g_assert_true(g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD |
                                G_SPAWN_STDOUT_TO_DEV_NULL |
                                G_SPAWN_STDERR_TO_DEV_NULL,
                                NULL, NULL, &vm.pid, NULL));
    for (int i = 0; i < 100 && !g_file_test(sock, G_FILE_TEST_EXISTS); i++) {
        g_usleep(G_USEC_PER_SEC / 20);
    }
    vm.fd = unix_connect(sock, NULL);
    g_assert_cmpint(vm.fd, >=, 0);
    greeting = qmp_fd_receive(vm.fd);
    qobject_unref(qmp_fd(vm.fd, "{'execute': 'qmp_capabilities'}"));
    return vm;
}

static void vm_stop(Vm *vm)
{
    kill(vm->pid, SIGKILL);
    waitpid(vm->pid, NULL, 0);
    g_spawn_close_pid(vm->pid);
    close(vm->fd);
}

/* Read the word the guest stored, with the guest stopped */
static uint32_t vm_out(Vm *vm)
{
    g_autofree char *cmd = g_strdup_printf("xp/1wx 0x%x", OUT_ADDR);
    QDict *rsp = vm_qmp(vm, "{'execute': 'human-monitor-command',"
                        " 'arguments': {'command-line': %s}}", cmd);
    const char *s = qdict_get_str(rsp, "return");
    uint32_t v;

    g_assert_nonnull(strchr(s, ':'));
    v = g_ascii_strtoull(strchr(s, ':') + 1, NULL, 16);
    qobject_unref(rsp);
    return v;
}

static void test_dacr_secure(void)
{
    g_autofree char *dir = g_dir_make_tmp("arm-cpreg-XXXXXX", NULL);
    g_autofree char *snap = g_strdup_printf("%s/snap", dir);
    g_autofree char *extra = NULL;
    g_autofree char *incoming = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *bin = g_strdup_printf("%s/guest.bin", dir);
    uint32_t guest_le[G_N_ELEMENTS(guest)];
    Vm src, dst;
    uint32_t v;

    for (int i = 0; i < G_N_ELEMENTS(guest); i++) {
        guest_le[i] = GUINT32_TO_LE(guest[i]);
    }
    g_assert_true(g_file_set_contents(bin, (char *)guest_le,
                                      sizeof(guest_le), NULL));
    extra = g_strdup_printf("-device loader,file=%s,addr=0x%x,cpu-num=0",
                            bin, RAM_BASE);
    src = vm_start(dir, "src", extra);

    /* Let the guest run until DACR has been stored */
    for (int i = 0; i < 100; i++) {
        g_usleep(G_USEC_PER_SEC / 20);
        qobject_unref(vm_qmp(&src, "{'execute': 'stop'}"));
        v = vm_out(&src);
        if (v == DACR_VAL) {
            break;
        }
        qobject_unref(vm_qmp(&src, "{'execute': 'cont'}"));
    }
    g_assert_cmphex(v, ==, DACR_VAL);

    uri = g_strdup_printf("exec:cat > %s", snap);
    qobject_unref(vm_qmp(&src, "{'execute': 'migrate',"
                         " 'arguments': {'uri': %s}}", uri));
    for (;;) {
        QDict *rsp = vm_qmp(&src, "{'execute': 'query-migrate'}");
        const char *st = qdict_get_try_str(qdict_get_qdict(rsp, "return"),
                                           "status");
        bool done = st && !strcmp(st, "completed");

        g_assert_false(st && !strcmp(st, "failed"));
        qobject_unref(rsp);
        if (done) {
            break;
        }
        g_usleep(G_USEC_PER_SEC / 20);
    }
    vm_stop(&src);

    incoming = g_strdup_printf("%s -incoming 'exec:cat < %s'", extra, snap);
    dst = vm_start(dir, "dst", incoming);
    for (;;) {
        QDict *rsp = vm_qmp(&dst, "{'execute': 'query-status'}");
        const char *st = qdict_get_str(qdict_get_qdict(rsp, "return"),
                                       "status");
        bool loaded = strcmp(st, "inmigrate");

        qobject_unref(rsp);
        if (loaded) {
            break;
        }
        g_usleep(G_USEC_PER_SEC / 20);
    }
    /* The guest rewrites the word, so it shows what DACR reads after restore */
    qobject_unref(vm_qmp(&dst, "{'execute': 'cont'}"));
    g_usleep(G_USEC_PER_SEC / 2);
    qobject_unref(vm_qmp(&dst, "{'execute': 'stop'}"));
    g_assert_cmphex(vm_out(&dst), ==, DACR_VAL);
    vm_stop(&dst);

    g_remove(snap);
    g_remove(bin);
    g_rmdir(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/arm/cpreg-migrate/dacr-secure", test_dacr_secure);
    return g_test_run();
}
