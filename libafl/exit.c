#include "qemu/osdep.h"
#include "cpu.h"
#include "tcg/tcg.h"
#include "tcg/tcg-op-common.h"
#include "exec/cpu-common.h"
#include "qemu/log.h"

#include "libafl/exit.h"
#include "libafl/defs.h"
#include "libafl/cpu.h"

#if !defined(CONFIG_USER_ONLY) && defined(AS_LIB)
#include "system/runstate.h"
#endif

#ifdef CONFIG_USER_ONLY
#define THREAD_MODIFIER __thread
#else
#define THREAD_MODIFIER
#endif

struct libafl_breakpoint* libafl_qemu_breakpoints = NULL;

int libafl_qemu_set_breakpoint(vaddr pc)
{
    CPUState* cpu;

    CPU_FOREACH(cpu) { libafl_breakpoint_invalidate(cpu, pc); }

    struct libafl_breakpoint* bp = calloc(sizeof(struct libafl_breakpoint), 1);
    bp->addr = pc;
    bp->next = libafl_qemu_breakpoints;
    libafl_qemu_breakpoints = bp;
    return 1;
}

/*
 * A translated block keeps its breakpoint check until the queued TB flush
 * runs, so the helper asks here whether pc is still armed.
 */
bool libafl_qemu_breakpoint_armed(vaddr pc)
{
    for (struct libafl_breakpoint* bp = libafl_qemu_breakpoints; bp;
         bp = bp->next) {
        if (bp->addr == pc) {
            return true;
        }
    }
    return false;
}

int libafl_qemu_remove_breakpoint(vaddr pc)
{
    CPUState* cpu;
    int r = 0;

    struct libafl_breakpoint** bp = &libafl_qemu_breakpoints;
    while (*bp) {
        if ((*bp)->addr == pc) {
            CPU_FOREACH(cpu) { libafl_breakpoint_invalidate(cpu, pc); }

            *bp = (*bp)->next;
            r = 1;
        } else {
            bp = &(*bp)->next;
        }
    }
    return r;
}

static THREAD_MODIFIER struct libafl_exit_reason last_exit_reason;
static THREAD_MODIFIER bool expected_exit = false;

#if defined(TARGET_ARM)
#define THUMB_MASK(cpu, value) (value | cpu_env(cpu)->thumb)
#else
#define THUMB_MASK(cpu, value) value
#endif

// called before exiting the cpu exec with the custom exception
void libafl_sync_exit_cpu(void)
{
    if (last_exit_reason.next_pc && last_exit_reason.cpu) {
        CPUClass* cc = CPU_GET_CLASS(last_exit_reason.cpu);
        cc->set_pc(last_exit_reason.cpu,
                   THUMB_MASK(last_exit_reason.cpu, last_exit_reason.next_pc));
    }
    last_exit_reason.next_pc = 0;
}

bool libafl_exit_asap(void) { return expected_exit; }

static void prepare_qemu_exit(CPUState* cpu, vaddr next_pc)
{
    expected_exit = true;
    last_exit_reason.cpu = cpu;
    last_exit_reason.next_pc = next_pc;

#if !defined(CONFIG_USER_ONLY) && defined(AS_LIB)
    qemu_system_return_request();
#endif

    // in usermode, this may be called from the syscall hook, thus already out
    // of the cpu_exec but still in the cpu_loop
    if (cpu->running) {
        cpu->exception_index = EXCP_LIBAFL_EXIT;
        cpu_loop_exit(cpu);
    }
}

CPUState* libafl_last_exit_cpu(void)
{
    if (expected_exit) {
        return last_exit_reason.cpu;
    }

    return NULL;
}

void libafl_exit_request_internal(CPUState* cpu, vaddr pc,
                                  ShutdownCause cause, int signal)
{
    last_exit_reason.kind = INTERNAL;
    last_exit_reason.data.internal.cause = cause;
    last_exit_reason.data.internal.signal = signal;

    last_exit_reason.cpu = cpu;
    last_exit_reason.next_pc = pc;
    expected_exit = true;
}

void libafl_exit_request_custom_insn(CPUState* cpu, vaddr pc,
                                     enum libafl_custom_insn_kind kind)
{
    last_exit_reason.kind = CUSTOM_INSN;

    prepare_qemu_exit(cpu, pc);
}

void libafl_exit_request_breakpoint(CPUState* cpu, vaddr pc)
{
    last_exit_reason.kind = BREAKPOINT;
    last_exit_reason.data.breakpoint.addr = pc;

    /* Debug aid (-d unimp): the armed list for the first few hits only */
    static int logged;
    if (logged < 4 && qemu_loglevel_mask(LOG_UNIMP)) {
        logged++;
        qemu_log("libafl: breakpoint exit at 0x%" VADDR_PRIx ", armed:", pc);
        for (struct libafl_breakpoint* b = libafl_qemu_breakpoints; b;
             b = b->next) {
            qemu_log(" 0x%" VADDR_PRIx, b->addr);
        }
        qemu_log("\n");
    }

    prepare_qemu_exit(cpu, pc);
}

void libafl_exit_request_crash(CPUState* cpu)
{
    CPUClass* cc = CPU_GET_CLASS(cpu);

    expected_exit = true;
    last_exit_reason.kind = CRASH;
    last_exit_reason.cpu = cpu;

    prepare_qemu_exit(current_cpu, cc->get_pc(cpu));
}

#ifndef CONFIG_USER_ONLY
void libafl_exit_request_timeout(void)
{
    expected_exit = true;
    last_exit_reason.kind = TIMEOUT;
    last_exit_reason.cpu = current_cpu;
    last_exit_reason.next_pc = 0;

#ifdef AS_LIB
    qemu_system_return_request();
#endif
}
#endif

void libafl_qemu_trigger_breakpoint(CPUState* cpu)
{
    CPUClass* cc = CPU_GET_CLASS(cpu);
    libafl_exit_request_breakpoint(cpu, cc->get_pc(cpu));
}

void libafl_exit_signal_vm_start(void)
{
    last_exit_reason.cpu = NULL;
    last_exit_reason.next_pc = 0;
    expected_exit = false;
}

/*
 * Debug aid (-d unimp): count how an exit was produced and print the count
 * at powers of two, with the cpu singlestep state, so a flood of identical
 * exits can be told from a helper hit and from a stale reason.
 */
void libafl_log_exit_path(const char* what, vaddr pc)
{
    static unsigned long n[3];
    static const char* names[3] = { "helper armed hit",
                                    "helper ignored (not armed)",
                                    "exit reason read" };
    int i = what[0] == 'h' ? (what[7] == 'a' ? 0 : 1) : 2;

    if (!qemu_loglevel_mask(LOG_UNIMP)) {
        return;
    }
    n[i]++;
    if ((n[i] & (n[i] - 1)) == 0) {
        CPUState* cpu = first_cpu;
        qemu_log("libafl: %s #%lu pc=0x%" VADDR_PRIx " singlestep=%d "
                 "expected_exit=%d kind=%d\n", names[i], n[i], pc,
                 cpu ? (int)cpu->singlestep_enabled : -1, expected_exit,
                 (int)last_exit_reason.kind);
    }
}

struct libafl_exit_reason* libafl_get_exit_reason(void)
{
    if (expected_exit) {
        libafl_log_exit_path("exit reason read", last_exit_reason.next_pc);
        return &last_exit_reason;
    }

    return NULL;
}

void libafl_qemu_breakpoint_run(vaddr pc_next)
{
    struct libafl_breakpoint* bp = libafl_qemu_breakpoints;
    while (bp) {
        if (bp->addr == pc_next) {
            TCGv_i64 tmp0 = tcg_constant_i64((uint64_t)pc_next);
            gen_helper_libafl_qemu_handle_breakpoint(tcg_env, tmp0);
        }
        bp = bp->next;
    }
}
