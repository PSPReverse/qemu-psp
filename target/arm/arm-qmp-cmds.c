/*
 * QEMU monitor.c for ARM.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/boards.h"
#include "kvm_arm.h"
#include "monitor/monitor.h"
#include "monitor/hmp-target.h"
#include "monitor/hmp.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qapi/qobject-input-visitor.h"
#include "qapi/qapi-commands-machine-target.h"
#include "qapi/qapi-commands-misc-target.h"
#include "qobject/qdict.h"
#include "qom/qom-qobject.h"
#include "target/arm/internals.h"

static GICCapability *gic_cap_new(int version)
{
    GICCapability *cap = g_new0(GICCapability, 1);
    cap->version = version;
    /* by default, support none */
    cap->emulated = false;
    cap->kernel = false;
    return cap;
}

static inline void gic_cap_kvm_probe(GICCapability *v2, GICCapability *v3)
{
#ifdef CONFIG_KVM
    int fdarray[3];

    if (!kvm_arm_create_scratch_host_vcpu(NULL, fdarray, NULL)) {
        return;
    }

    /* Test KVM GICv2 */
    if (kvm_device_supported(fdarray[1], KVM_DEV_TYPE_ARM_VGIC_V2)) {
        v2->kernel = true;
    }

    /* Test KVM GICv3 */
    if (kvm_device_supported(fdarray[1], KVM_DEV_TYPE_ARM_VGIC_V3)) {
        v3->kernel = true;
    }

    kvm_arm_destroy_scratch_host_vcpu(fdarray);
#endif
}

GICCapabilityList *qmp_query_gic_capabilities(Error **errp)
{
    GICCapabilityList *head = NULL;
    GICCapability *v2 = gic_cap_new(2), *v3 = gic_cap_new(3);

    v2->emulated = true;
    v3->emulated = true;

    gic_cap_kvm_probe(v2, v3);

    QAPI_LIST_PREPEND(head, v2);
    QAPI_LIST_PREPEND(head, v3);

    return head;
}

QEMU_BUILD_BUG_ON(ARM_MAX_VQ > 16);

/*
 * These are cpu model features we want to advertise. The order here
 * matters as this is the order in which qmp_query_cpu_model_expansion
 * will attempt to set them. If there are dependencies between features,
 * then the order that considers those dependencies must be used.
 */
static const char *cpu_model_advertised_features[] = {
    "aarch64", "pmu", "sve",
    "sve128", "sve256", "sve384", "sve512",
    "sve640", "sve768", "sve896", "sve1024", "sve1152", "sve1280",
    "sve1408", "sve1536", "sve1664", "sve1792", "sve1920", "sve2048",
    "kvm-no-adjvtime", "kvm-steal-time",
    "pauth", "pauth-impdef", "pauth-qarma3", "pauth-qarma5",
    NULL
};

CpuModelExpansionInfo *qmp_query_cpu_model_expansion(CpuModelExpansionType type,
                                                     CpuModelInfo *model,
                                                     Error **errp)
{
    CpuModelExpansionInfo *expansion_info;
    const QDict *qdict_in;
    QDict *qdict_out;
    ObjectClass *oc;
    Object *obj;
    const char *name;
    int i;

    if (type != CPU_MODEL_EXPANSION_TYPE_FULL) {
        error_setg(errp, "The requested expansion type is not supported");
        return NULL;
    }

    if (!kvm_enabled() && !strcmp(model->name, "host")) {
        error_setg(errp, "The CPU type '%s' requires KVM", model->name);
        return NULL;
    }

    oc = cpu_class_by_name(TYPE_ARM_CPU, model->name);
    if (!oc) {
        error_setg(errp, "The CPU type '%s' is not a recognized ARM CPU type",
                   model->name);
        return NULL;
    }

    if (kvm_enabled()) {
        bool supported = false;

        if (!strcmp(model->name, "host") || !strcmp(model->name, "max")) {
            /* These are kvmarm's recommended cpu types */
            supported = true;
        } else if (current_machine->cpu_type) {
            const char *cpu_type = current_machine->cpu_type;
            int len = strlen(cpu_type) - strlen(ARM_CPU_TYPE_SUFFIX);

            if (strlen(model->name) == len &&
                !strncmp(model->name, cpu_type, len)) {
                /* KVM is enabled and we're using this type, so it works. */
                supported = true;
            }
        }
        if (!supported) {
            error_setg(errp, "We cannot guarantee the CPU type '%s' works "
                             "with KVM on this host", model->name);
            return NULL;
        }
    }

    obj = object_new(object_class_get_name(oc));

    if (model->props) {
        Visitor *visitor;
        Error *err = NULL;

        visitor = qobject_input_visitor_new(model->props);
        if (!visit_start_struct(visitor, "model.props", NULL, 0, errp)) {
            visit_free(visitor);
            object_unref(obj);
            return NULL;
        }

        qdict_in = qobject_to(QDict, model->props);
        i = 0;
        while ((name = cpu_model_advertised_features[i++]) != NULL) {
            if (qdict_get(qdict_in, name)) {
                if (!object_property_set(obj, name, visitor, &err)) {
                    break;
                }
            }
        }

        if (!err) {
            visit_check_struct(visitor, &err);
        }
        if (!err) {
            arm_cpu_finalize_features(ARM_CPU(obj), &err);
        }
        visit_end_struct(visitor, NULL);
        visit_free(visitor);
        if (err) {
            object_unref(obj);
            error_propagate(errp, err);
            return NULL;
        }
    } else {
        arm_cpu_finalize_features(ARM_CPU(obj), &error_abort);
    }

    expansion_info = g_new0(CpuModelExpansionInfo, 1);
    expansion_info->model = g_malloc0(sizeof(*expansion_info->model));
    expansion_info->model->name = g_strdup(model->name);

    qdict_out = qdict_new();

    i = 0;
    while ((name = cpu_model_advertised_features[i++]) != NULL) {
        ObjectProperty *prop = object_property_find(obj, name);
        if (prop) {
            QObject *value;

            assert(prop->get);
            value = object_property_get_qobject(obj, name, &error_abort);

            qdict_put_obj(qdict_out, name, value);
        }
    }

    if (!qdict_size(qdict_out)) {
        qobject_unref(qdict_out);
    } else {
        expansion_info->model->props = QOBJECT(qdict_out);
    }

    object_unref(obj);

    return expansion_info;
}

static void arm_cpu_add_definition(gpointer data, gpointer user_data)
{
    ObjectClass *oc = data;
    CpuDefinitionInfoList **cpu_list = user_data;
    CpuDefinitionInfo *info;
    const char *typename;

    typename = object_class_get_name(oc);
    info = g_malloc0(sizeof(*info));
    info->name = cpu_model_from_type(typename);
    info->q_typename = g_strdup(typename);

    QAPI_LIST_PREPEND(*cpu_list, info);
}

CpuDefinitionInfoList *qmp_query_cpu_definitions(Error **errp)
{
    CpuDefinitionInfoList *cpu_list = NULL;
    GSList *list;

    list = object_class_get_list(TYPE_ARM_CPU, false);
    g_slist_foreach(list, arm_cpu_add_definition, &cpu_list);
    g_slist_free(list);

    return cpu_list;
}

typedef struct Mapping {
    hwaddr addr;
    bool k_exec;
    bool k_read;
    bool k_write;

    bool u_exec;
    bool u_read;
    bool u_write;
} Mapping;

static uint8_t get_mapping(CPUARMState *env, vaddr addr, Mapping *mapping)
{
    // set default mapping
    *mapping = (Mapping){0};
    uint8_t lg_page_size = 12; // 1 << 12 = 4KB

#define UPDATE_PART(MEMOP, MMUIDX, FIELD_NAME)                                   \
    do {                                                                       \
      GetPhysAddrResult result = {0};                                          \
      ARMMMUFaultInfo fi = {0};                                                \
      bool failed = get_phys_addr_with_space_nogpc(                            \
          env, addr, MEMOP, MO_8, MMUIDX, ARMSS_Secure, &result, &fi);   \
      if (!failed) {                                                           \
        mapping->FIELD_NAME = true;                                            \
        mapping->addr = result.f.phys_addr;                                    \
        lg_page_size = result.f.lg_page_size;                                  \
      }                                                                        \
    } while (false)

    UPDATE_PART(MMU_DATA_STORE, ARMMMUIdx_E3, k_write);
    UPDATE_PART(MMU_DATA_LOAD, ARMMMUIdx_E3, k_read);
    UPDATE_PART(MMU_INST_FETCH, ARMMMUIdx_E3, k_exec);
   
    UPDATE_PART(MMU_DATA_STORE, ARMMMUIdx_E30_0, u_write);
    UPDATE_PART(MMU_DATA_LOAD, ARMMMUIdx_E30_0, u_read);
    UPDATE_PART(MMU_INST_FETCH, ARMMMUIdx_E30_0, u_exec);

    return lg_page_size;
}

static void get_permissions_str(Mapping *mapping, char (*str)[8]){
    (*str)[0] = mapping->k_read ? 'r' : '_';
    (*str)[1] = mapping->k_write ? 'w' : '_';
    (*str)[2] = mapping->k_exec ? 'x' : '_';
    
    (*str)[3] = ' ';

    (*str)[4] = mapping->u_read ? 'r' : '_';
    (*str)[5] = mapping->u_write ? 'w' : '_';
    (*str)[6] = mapping->u_exec ? 'x' : '_';

    (*str)[7] = '\0';
}

static bool mapping_is_invalid(Mapping *m) {
    return m->addr == 0 && !m->k_read && !m->k_write && !m->k_exec &&
           !m->u_read && !m->u_write && !m->u_exec;
}

static bool mapping_is_in_seqence(Mapping *prev, Mapping *curr, hwaddr step){
    if (mapping_is_invalid(prev) && mapping_is_invalid(curr)) {
      return true;
    }

    if(prev->addr + step != curr->addr) {
        return false;
    }

#define HAS_TO_BE_SAME(NAME)                                                   \
    if (prev->NAME != curr->NAME) {                                            \
      return false;                                                            \
    }

    HAS_TO_BE_SAME(k_read)
    HAS_TO_BE_SAME(k_write)
    HAS_TO_BE_SAME(k_exec)
    HAS_TO_BE_SAME(u_read)
    HAS_TO_BE_SAME(u_write)
    HAS_TO_BE_SAME(u_exec)

    return true;
}

static vaddr max_vaddr = (1024ul * 1024ul * 1024ul * 4ul) - 1;

void hmp_info_mem(Monitor *mon, const QDict *qdict)
{
    CPUARMState *env;

    env = mon_get_cpu_env(mon);
    if (!env) {
        monitor_printf(mon, "No CPU available\n");
        return;
    }

    monitor_printf(mon, "Virtual Address Range      Physical   Kernel User\n");

    vaddr addr = 0;

    struct Mapping mapping;
    char perms_str[8];

    struct Mapping prev_mapping;
    
    monitor_printf(mon, "0x%08"VADDR_PRIx" - ", addr);

    addr += 1 << get_mapping(env, addr, &prev_mapping);
    hwaddr prev_mapping_target = prev_mapping.addr; 

    while(true) {
        hwaddr addr_step = 1 << get_mapping(env, addr, &mapping);
        hwaddr next_addr = addr + addr_step;

        if(!mapping_is_in_seqence(&prev_mapping, &mapping, addr_step) || next_addr >= max_vaddr) {
            if(next_addr > max_vaddr) {
                next_addr = max_vaddr;
            }
            monitor_printf(mon, "0x%08" VADDR_PRIx " -> ", next_addr);

            if(mapping_is_invalid(&prev_mapping)) {
                monitor_printf(mon, "n/a\n");    
            } else {
                get_permissions_str(&prev_mapping, &perms_str);
                monitor_printf(mon, "0x%08" HWADDR_PRIX " %s\n", prev_mapping_target, perms_str);
            }

            if(next_addr >= max_vaddr) {
                break;
            }

            monitor_printf(mon, "0x%08" VADDR_PRIx " - ", next_addr + addr);

            prev_mapping_target = mapping.addr;
            
        }

        prev_mapping = mapping;
        addr = next_addr;
    }
}
