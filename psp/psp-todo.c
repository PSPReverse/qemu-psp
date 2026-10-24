/*
 * Emulator gaps: counting, per-iteration bitmap and policy of PSP_TODO().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"

#include "psp/psp-todo.h"

#include "hw/core/cpu.h"
#include "qapi/error.h"
#include "qapi/qapi-types-qom.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/rcu.h"
#include "qom/object_interfaces.h"
#include "trace.h"

#if !defined(CONFIG_USER_ONLY) && defined(AS_LIB)
#include "system/runstate.h"
#endif

/*
 * Host-side state, deliberately not in vmstate: it must survive the snapshot
 * restores the harness does between iterations. Updated with atomics, so hits
 * and the aspfuzz_todo_* functions need no lock.
 */
static int psp_todo_policy = PSP_TODO_POLICY_LOG;
static bool psp_todo_stop;

/* Filled by the linker from the PSP_TODO_SECTION entries; weak if none */
extern PspTodoSite* const __start_psp_todo_sites[] __attribute__((weak));
extern PspTodoSite* const __stop_psp_todo_sites[] __attribute__((weak));

static uint32_t psp_todo_nsites;
/* Per-iteration bitmap, DIV_ROUND_UP(psp_todo_nsites, 64) words */
static uint64_t* psp_todo_hits;

/*
 * Index i is the position of the site in the section, so indexes are stable
 * for a given binary and known before the first hit. Runs before main(), where
 * error_report() is not usable yet (it needs the monitor lock), so duplicate
 * names are reported with fprintf().
 */
static void __attribute__((constructor)) psp_todo_index_sites(void)
{
    PspTodoSite* const* sites = __start_psp_todo_sites;
    uint32_t n;

    if (sites == NULL) {
        return;
    }
    n = __stop_psp_todo_sites - sites;
    psp_todo_hits = g_new0(uint64_t, DIV_ROUND_UP(n, 64));
    for (uint32_t i = 0; i < n; i++) {
        sites[i]->index = i;
        for (uint32_t j = 0; j < i; j++) {
            if (strcmp(sites[i]->site, sites[j]->site) == 0) {
                fprintf(stderr,
                        "psp-todo: site name %s is not unique (%s:%d and "
                        "%s:%d)\n",
                        sites[i]->site, sites[j]->file, sites[j]->line,
                        sites[i]->file, sites[i]->line);
            }
        }
    }
    psp_todo_nsites = n;
}

/*
 * Make the vCPU leave its execution loop. cpu_exit() only sets flags and
 * signals, so this never blocks. Outside a vCPU thread (timer, bottom half)
 * current_cpu is NULL, so every vCPU is kicked.
 */
static void psp_todo_kick(void)
{
    CPUState* cpu = current_cpu;

    if (cpu != NULL) {
        cpu_exit(cpu);
        return;
    }
    WITH_RCU_READ_LOCK_GUARD() {
        CPU_FOREACH(cpu) {
            cpu_exit(cpu);
        }
    }
}

void psp_todo_hit(PspTodoSite* site, const char* fmt, ...)
{
    int policy = qatomic_read(&psp_todo_policy);
    bool log =
        policy != PSP_TODO_POLICY_IGNORE && qemu_loglevel_mask(LOG_UNIMP);

    qatomic_inc(&site->count);
    /* PSP_TODO_NO_INDEX: no linker section, the hit is only counted */
    if (site->index < psp_todo_nsites) {
        qatomic_or(&psp_todo_hits[site->index / 64],
                   1ULL << (site->index % 64));
    }

    if (log || trace_event_get_state_backends(TRACE_PSP_TODO_HIT)) {
        g_autofree char* msg = NULL;
        va_list ap;

        va_start(ap, fmt);
        msg = g_strdup_vprintf(fmt, ap);
        va_end(ap);
        trace_psp_todo_hit(site->site, msg, site->ret_doc);
        if (log) {
            qemu_log_mask(LOG_UNIMP,
                          "PSP_TODO %s: %s (guest sees: %s) %s:%d\n",
                          site->site, msg, site->ret_doc, site->file,
                          site->line);
        }
    }

    /*
     * Same return path as a LibAFL exit (libafl/exit.c): the main loop stops
     * the VM and the run returns to the harness. Only the first stop hit after
     * aspfuzz_todo_reset() requests it, so further hits before the harness
     * looks cannot leave a request pending for the next run. Without AS_LIB
     * there is no harness to return to, so only the flag and the kick remain.
     * The xchg is a full barrier, so a reader that sees the flag with
     * aspfuzz_todo_stop_requested() also sees this hit's count and bit.
     */
    if (policy == PSP_TODO_POLICY_STOP) {
        if (!qatomic_xchg(&psp_todo_stop, true)) {
#if !defined(CONFIG_USER_ONLY) && defined(AS_LIB)
            qemu_system_return_request();
#endif
        }
        psp_todo_kick();
    }
}

/** BEGIN ASPFuzz */
/*
 * Clear the per-iteration bitmap and the stop request. The per-site totals
 * are kept. Returns 0.
 */
int32_t aspfuzz_todo_reset(void);
int32_t aspfuzz_todo_reset(void)
{
    for (uint32_t i = 0; i < DIV_ROUND_UP(psp_todo_nsites, 64); i++) {
        qatomic_set(&psp_todo_hits[i], 0);
    }
    qatomic_set(&psp_todo_stop, false);
    return 0;
}

/*
 * 1 if a hit with policy stop happened since the last aspfuzz_todo_reset();
 * else 0.
 */
int32_t aspfuzz_todo_stop_requested(void);
int32_t aspfuzz_todo_stop_requested(void)
{
    return qatomic_load_acquire(&psp_todo_stop);
}

/*
 * Copy the sites hit since the last aspfuzz_todo_reset() to bitmap: bit i % 64
 * of word i / 64 is site i of aspfuzz_todo_site(). *nsites receives the number
 * of sites, also when the bitmap is too short, so words == 0 queries it.
 * Words past the last site are zeroed. Returns the number of sites hit, or -1
 * if nsites is NULL, bitmap is NULL with words > 0, or words * 64 < *nsites.
 */
int32_t aspfuzz_todo_status(uint64_t* bitmap, uint32_t words,
                            uint32_t* nsites);
int32_t aspfuzz_todo_status(uint64_t* bitmap, uint32_t words, uint32_t* nsites)
{
    uint32_t used = DIV_ROUND_UP(psp_todo_nsites, 64);
    int32_t hit = 0;

    if (nsites == NULL) {
        return -1;
    }
    *nsites = psp_todo_nsites;
    if ((bitmap == NULL && words > 0) || words < used) {
        return -1;
    }
    for (uint32_t i = 0; i < words; i++) {
        bitmap[i] = i < used ? qatomic_read(&psp_todo_hits[i]) : 0;
        hit += ctpop64(bitmap[i]);
    }
    return hit;
}

/*
 * Name, guest-visible behaviour (the PSP_TODO ret_doc) and total hits since
 * startup of site idx. NULL output pointers are skipped. Returns -1 if idx is
 * not below the number of sites; else 0.
 */
int32_t aspfuzz_todo_site(uint32_t idx, const char** name,
                          const char** ret_doc, uint64_t* count);
int32_t aspfuzz_todo_site(uint32_t idx, const char** name,
                          const char** ret_doc, uint64_t* count)
{
    const PspTodoSite* site;

    if (idx >= psp_todo_nsites) {
        return -1;
    }
    site = __start_psp_todo_sites[idx];
    if (name != NULL) {
        *name = site->site;
    }
    if (ret_doc != NULL) {
        *ret_doc = site->ret_doc;
    }
    if (count != NULL) {
        *count = qatomic_read(&site->count);
    }
    return 0;
}
/** END ASPFuzz */

/*
 * -object psp-todo,id=todo,policy=ignore|log|stop (schema: PspTodoProperties in
 * qapi/qom.json). At most one instance; while it exists its policy is the
 * global one, without it the policy is log.
 */
#define TYPE_PSP_TODO_OBJECT "psp-todo"
OBJECT_DECLARE_SIMPLE_TYPE(PspTodoObject, PSP_TODO_OBJECT)

struct PspTodoObject {
    Object parent_obj;
    int policy;
};

static PspTodoObject* psp_todo_obj;

static int psp_todo_get_policy(Object* obj, Error** errp)
{
    return PSP_TODO_OBJECT(obj)->policy;
}

static void psp_todo_set_policy(Object* obj, int value, Error** errp)
{
    PspTodoObject* t = PSP_TODO_OBJECT(obj);

    t->policy = value;
    if (psp_todo_obj == t) {
        qatomic_set(&psp_todo_policy, value);
    }
}

static void psp_todo_complete(UserCreatable* uc, Error** errp)
{
    PspTodoObject* t = PSP_TODO_OBJECT(uc);

    if (psp_todo_obj != NULL) {
        error_setg(errp, "only one %s object is allowed",
                   TYPE_PSP_TODO_OBJECT);
        return;
    }
    psp_todo_obj = t;
    qatomic_set(&psp_todo_policy, t->policy);
}

static void psp_todo_instance_init(Object* obj)
{
    PSP_TODO_OBJECT(obj)->policy = PSP_TODO_POLICY_LOG;
}

static void psp_todo_instance_finalize(Object* obj)
{
    if (psp_todo_obj == PSP_TODO_OBJECT(obj)) {
        psp_todo_obj = NULL;
        qatomic_set(&psp_todo_policy, PSP_TODO_POLICY_LOG);
    }
}

static void psp_todo_class_init(ObjectClass* oc, const void* data)
{
    UserCreatableClass* ucc = USER_CREATABLE_CLASS(oc);

    ucc->complete = psp_todo_complete;
    object_class_property_add_enum(oc, "policy", "PspTodoPolicy",
                                   &PspTodoPolicy_lookup, psp_todo_get_policy,
                                   psp_todo_set_policy);
    object_class_property_set_description(
        oc, "policy", "What a PSP_TODO hit does: ignore, log or stop");
}

static const TypeInfo psp_todo_info = {
    .name = TYPE_PSP_TODO_OBJECT,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(PspTodoObject),
    .instance_init = psp_todo_instance_init,
    .instance_finalize = psp_todo_instance_finalize,
    .class_init = psp_todo_class_init,
    .interfaces = (const InterfaceInfo[]){{TYPE_USER_CREATABLE}, {}},
};

static void psp_todo_register_types(void)
{
    type_register_static(&psp_todo_info);
}
type_init(psp_todo_register_types);
