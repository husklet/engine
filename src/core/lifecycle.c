#include "engine_backend.h"
#include "options.h"

#include <stdio.h>

#ifndef HL_PRODUCTION_GUEST_ISA
#error HL_PRODUCTION_GUEST_ISA is required
#endif

int hl_run_linux_guest(const hl_host_services *host, hl_linux_abi *box, const char *rootfs, uint32_t argc,
                       char *const argv[]);

typedef struct hl_production_entry_context {
    const hl_engine_config *config;
    uint32_t argc;
    const char *const *argv;
    const hl_host_services *host;
    hl_linux_abi *box;
    hl_options *options;
} hl_production_entry_context;

static int32_t hl_production_entry(void *opaque) {
    hl_production_entry_context *context = opaque;
    hl_options *previous = hl_options_bind_process(context->options);
    int32_t result = hl_run_linux_guest(context->host, context->box, context->config->rootfs, context->argc,
                                        (char *const *)(uintptr_t)context->argv);
    (void)hl_options_bind_process(previous);
    return result;
}

static hl_status hl_production_start_process(const hl_host_services *host, hl_linux_abi *box,
                                             hl_options *options,
                                             const hl_engine_config *config, uint32_t argc,
                                             const char *const argv[], hl_host_handle *process) {
    hl_production_entry_context entry = {0};
    hl_host_result spawned;
    if (hl_host_services_validate(host, HL_HOST_CAP_PROCESS) != HL_STATUS_OK) return HL_STATUS_NOT_SUPPORTED;
    entry.config = config;
    entry.argc = argc;
    entry.argv = argv;
    entry.host = host;
    entry.box = box;
    entry.options = options;
    if (box == NULL) {
        spawned = host->process->spawn_cloned(host->context, hl_production_entry, &entry);
    } else {
        return hl_linux_abi_spawn(box, hl_production_entry, &entry, process);
    }
    if (spawned.status != HL_STATUS_OK) return (hl_status)spawned.status;
    *process = spawned.value;
    return HL_STATUS_OK;
}

static const hl_engine_backend backend = {HL_PRODUCTION_GUEST_ISA, hl_production_start_process};

__attribute__((constructor)) static void hl_production_register_backend(void) {
    hl_engine_backend_register(&backend);
}
