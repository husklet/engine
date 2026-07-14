#include "engine_backend.h"

#include <stdlib.h>

#ifndef HL_PRODUCTION_GUEST_ISA
#error HL_PRODUCTION_GUEST_ISA is required
#endif

int hl_run_linux_guest(const hl_host_services *host, hl_linux_abi *box, const char *rootfs, uint32_t argc,
                       char *const argv[]);

typedef struct hl_production_entry_context {
    const char *rootfs;
    uint32_t argc;
    const char *const *argv;
    const hl_host_services *host;
    hl_linux_abi *box;
    hl_linux_fork_plan fork_plan;
} hl_production_entry_context;

static int32_t hl_production_entry(void *opaque) {
    hl_production_entry_context *context = opaque;
    if (context->box != NULL) {
        if (hl_linux_abi_fork_host_completed(&context->fork_plan) != HL_STATUS_OK ||
            hl_linux_abi_fork_child(context->box, &context->fork_plan) != HL_STATUS_OK)
            return 255;
    }
    return hl_run_linux_guest(context->host, context->box, context->rootfs, context->argc,
                              (char *const *)(uintptr_t)context->argv);
}

static hl_status hl_production_start_process(const hl_host_services *host, hl_linux_abi *box, const char *rootfs,
                                             uint32_t argc, const char *const argv[], hl_host_handle *process) {
    hl_production_entry_context entry = {0};
    hl_host_result spawned;
    hl_status completed;
    if (hl_host_services_validate(host, HL_HOST_CAP_PROCESS) != HL_STATUS_OK) return HL_STATUS_NOT_SUPPORTED;
    entry.rootfs = rootfs;
    entry.argc = argc;
    entry.argv = argv;
    entry.host = host;
    entry.box = box;
    if (box == NULL) {
        spawned = host->process->spawn_cloned(host->context, hl_production_entry, &entry);
    } else {
        entry.fork_plan.abi = HL_LINUX_ABI_VERSION;
        entry.fork_plan.size = sizeof(entry.fork_plan);
        entry.fork_plan.capacity = box->ofd_capacity;
        entry.fork_plan.records = calloc(box->ofd_capacity, sizeof(*entry.fork_plan.records));
        if (entry.fork_plan.records == NULL) return HL_STATUS_OUT_OF_MEMORY;
        completed = hl_linux_abi_fork_prepare(box, &entry.fork_plan);
        if (completed != HL_STATUS_OK) {
            free(entry.fork_plan.records);
            return completed;
        }
        spawned = host->process->spawn_prepared(host->context, hl_production_entry, &entry);
        completed = hl_linux_abi_fork_host_completed(&entry.fork_plan);
        if (completed == HL_STATUS_OK) completed = hl_linux_abi_fork_parent(box, &entry.fork_plan);
        free(entry.fork_plan.records);
        if (completed != HL_STATUS_OK) return completed;
    }
    if (spawned.status != HL_STATUS_OK) return (hl_status)spawned.status;
    *process = spawned.value;
    return HL_STATUS_OK;
}

static const hl_engine_backend backend = {HL_PRODUCTION_GUEST_ISA, hl_production_start_process};

__attribute__((constructor)) static void hl_production_register_backend(void) {
    hl_engine_backend_register(&backend);
}
