#include "../../core/engine_backend.h"

#ifndef HL_PRODUCTION_GUEST_ISA
#error HL_PRODUCTION_GUEST_ISA is required
#endif

int hl_run_linux_guest(const char *rootfs, int argc, char *const argv[]);

typedef struct hl_production_entry_context {
    const char *rootfs;
    int argc;
    const char *const *argv;
} hl_production_entry_context;

static int32_t hl_production_entry(void *opaque) {
    hl_production_entry_context *context = opaque;
    return hl_run_linux_guest(context->rootfs, context->argc, (char *const *)(uintptr_t)context->argv);
}

static hl_status hl_production_start_process(const hl_host_services *host, const char *rootfs, int argc,
                                             const char *const argv[], hl_host_handle *process) {
    hl_production_entry_context entry = {rootfs, argc, argv};
    hl_host_result spawned;
    if (hl_host_services_validate(host, HL_HOST_CAP_PROCESS) != HL_STATUS_OK) return HL_STATUS_NOT_SUPPORTED;
    spawned = host->process->spawn_cloned(host->context, hl_production_entry, &entry);
    if (spawned.status != HL_STATUS_OK) return (hl_status)spawned.status;
    *process = spawned.value;
    return HL_STATUS_OK;
}

static const hl_engine_backend backend = {HL_PRODUCTION_GUEST_ISA, hl_production_start_process};

__attribute__((constructor)) static void hl_production_register_backend(void) {
    hl_engine_backend_register(&backend);
}
