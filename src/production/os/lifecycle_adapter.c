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

static hl_status hl_production_run_process(const hl_host_services *host, const char *rootfs, int argc,
                                           const char *const argv[],
                                           hl_engine_exit *result) {
    hl_production_entry_context entry = {rootfs, argc, argv};
    hl_host_result spawned;
    hl_host_result waited;
    hl_host_result closed;
    if (hl_host_services_validate(host, HL_HOST_CAP_PROCESS) != HL_STATUS_OK) return HL_STATUS_NOT_SUPPORTED;
    spawned = host->process->spawn_cloned(host->context, hl_production_entry, &entry);
    if (spawned.status != HL_STATUS_OK) return (hl_status)spawned.status;
    waited = host->process->wait(host->context, spawned.value, HL_HOST_DEADLINE_INFINITE);
    closed = host->process->close(host->context, spawned.value);
    if (waited.status != HL_STATUS_OK) return (hl_status)waited.status;
    if (closed.status != HL_STATUS_OK) return (hl_status)closed.status;
    result->detail = 0;
    if (waited.detail == HL_HOST_PROCESS_EXIT_CODE) {
        result->kind = HL_ENGINE_EXIT_CODE;
        result->guest_status = (int32_t)waited.value;
    } else if (waited.detail == HL_HOST_PROCESS_EXIT_SIGNAL) {
        result->kind = HL_ENGINE_EXIT_SIGNAL;
        result->guest_status = (int32_t)waited.value;
    } else {
        result->kind = HL_ENGINE_EXIT_ENGINE_ERROR;
        result->guest_status = HL_STATUS_CORRUPT;
        return HL_STATUS_CORRUPT;
    }
    return HL_STATUS_OK;
}

static const hl_engine_backend backend = {HL_PRODUCTION_GUEST_ISA, hl_production_run_process};

__attribute__((constructor)) static void hl_production_register_backend(void) {
    hl_engine_backend_register(&backend);
}
