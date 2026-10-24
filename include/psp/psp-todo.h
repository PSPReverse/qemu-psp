/*
 * Emulator gaps: code paths where the model knowingly does not do what the
 * hardware does. See the PSP_TODO section of psp/DEBUG-KNOBS.md.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PSP_TODO_H
#define PSP_TODO_H

/* Index of a site that is not in the linker section (non-ELF builds) */
#define PSP_TODO_NO_INDEX UINT32_MAX

typedef struct PspTodoSite {
    const char* site;    /* unique name, e.g. "ccp.engine.unknown" */
    const char* ret_doc; /* what the guest sees while the gap is ignored */
    const char* file;
    int line;
    uint32_t index; /* bit in the per-iteration bitmap, set at startup */
    uint64_t count; /* hits since startup */
} PspTodoSite;

/*
 * Every PSP_TODO() puts a pointer to its descriptor in this section, so
 * psp-todo.c can number all sites at startup through the __start_/__stop_
 * symbols that ELF linkers provide. retain keeps the entries under
 * --gc-sections.
 */
#if __has_attribute(retain)
#define PSP_TODO_SECTION                                                       \
    __attribute__((section("psp_todo_sites"), used, retain))
#else
#define PSP_TODO_SECTION __attribute__((section("psp_todo_sites"), used))
#endif

/*
 * Report a hit of the emulator gap @site_, a unique string literal. @ret_doc_
 * is a string literal describing what the guest sees while the gap is ignored.
 * @fmt (no trailing newline) and its arguments describe this hit. The hit is
 * always counted; whether it is logged or stops the vCPU depends on the policy
 * of the psp-todo object.
 */
#define PSP_TODO(site_, ret_doc_, fmt, ...)                                    \
    do {                                                                       \
        static PspTodoSite psp_todo_site_ = {                                  \
            .site = "" site_ "",                                               \
            .ret_doc = "" ret_doc_ "",                                         \
            .file = __FILE__,                                                  \
            .line = __LINE__,                                                  \
            .index = PSP_TODO_NO_INDEX,                                        \
        };                                                                     \
        static PspTodoSite* const psp_todo_site_ptr_ PSP_TODO_SECTION =        \
            &psp_todo_site_;                                                   \
        psp_todo_hit(&psp_todo_site_, fmt, ##__VA_ARGS__);                     \
    } while (0)

void psp_todo_hit(PspTodoSite* site, const char* fmt, ...) G_GNUC_PRINTF(2, 3);

#endif
