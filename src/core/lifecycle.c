#include "target/namespace.h"
#include "engine_backend.h"
#include "engine_result.h"
#include "options.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef HL_PRODUCTION_GUEST_ISA
#error HL_PRODUCTION_GUEST_ISA is required
#endif

int hl_run_linux_guest(const hl_host_services *host, hl_linux_abi *box, const char *rootfs, uint32_t argc,
                       char *const argv[]);
hl_status hl_run_linux_guest_status(void);

typedef struct hl_production_result_state {
    hl_host_memory_mapping mapping;
    hl_engine_child_result *record;
} hl_production_result_state;

static hl_engine_child_result *active_result;
static int result_published;

void hl_engine_child_result_publish(int32_t guest_status, hl_status engine_status, uint64_t detail) {
    hl_engine_child_result record = {0, HL_ENGINE_CHILD_RESULT_VERSION, guest_status, engine_status, detail};
    if (result_published || active_result == NULL) return;
    result_published = 1;
    memcpy(active_result, &record, sizeof(record));
    atomic_store_explicit((_Atomic uint32_t *)&active_result->magic, HL_ENGINE_CHILD_RESULT_MAGIC,
                          memory_order_release);
}

void hl_engine_child_result_after_fork(void) {
    active_result = NULL;
    result_published = 1;
}

typedef struct hl_production_entry_context {
    const hl_engine_config *config;
    uint32_t argc;
    const char *const *argv;
    const hl_host_services *host;
    hl_linux_abi *box;
    hl_options *options;
    hl_engine_child_result *result;
} hl_production_entry_context;

static int32_t hl_production_entry(void *opaque) {
    hl_production_entry_context *context = opaque;
    active_result = context->result;
    result_published = 0;
    hl_options *previous = hl_options_bind_process(context->options);
    int32_t result = hl_run_linux_guest(context->host, context->box, context->config->rootfs, context->argc,
                                        (char *const *)(uintptr_t)context->argv);
    (void)hl_options_bind_process(previous);
    hl_engine_child_result_publish(result, hl_run_linux_guest_status(), 0);
    return result;
}

static void hl_production_result_release(const hl_host_services *host, hl_host_handle token) {
    hl_production_result_state *state = (hl_production_result_state *)(uintptr_t)token;
    if (state == NULL) return;
    if (state->mapping.handle != HL_HOST_HANDLE_INVALID)
        (void)host->memory->release(host->context, state->mapping.handle);
    free(state);
}

static hl_status hl_production_start_process(const hl_host_services *host, hl_linux_abi *box,
                                             hl_options *options,
                                             const hl_engine_config *config, uint32_t argc,
                                             const char *const argv[], hl_host_handle *process,
                                             hl_host_handle *result_token) {
    hl_production_entry_context entry = {0};
    hl_production_result_state *result;
    hl_host_result spawned;
    hl_host_result mapped;
    if (hl_host_services_validate(host, HL_HOST_CAP_PROCESS | HL_HOST_CAP_MEMORY) != HL_STATUS_OK)
        return HL_STATUS_NOT_SUPPORTED;
    *process = HL_HOST_HANDLE_INVALID;
    *result_token = HL_HOST_HANDLE_INVALID;
    result = calloc(1, sizeof(*result));
    if (result == NULL) return HL_STATUS_OUT_OF_MEMORY;
    result->mapping = (hl_host_memory_mapping){HL_HOST_MEMORY_MAPPING_ABI, sizeof(result->mapping), 0, 0, 0, 0};
    mapped = host->memory->map_anonymous(host->context, 0, sizeof(hl_engine_child_result),
                                         HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE, HL_HOST_MEMORY_SHARED,
                                         &result->mapping);
    if (mapped.status != HL_STATUS_OK) {
        free(result);
        return (hl_status)mapped.status;
    }
    result->record = (hl_engine_child_result *)(uintptr_t)result->mapping.address;
    memset(result->record, 0, sizeof(*result->record));
    entry.config = config;
    entry.argc = argc;
    entry.argv = argv;
    entry.host = host;
    entry.box = box;
    entry.options = options;
    entry.result = result->record;
    if (box == NULL) {
        spawned = host->process->spawn_cloned(host->context, hl_production_entry, &entry);
    } else {
        hl_status status = hl_linux_abi_spawn(box, hl_production_entry, &entry, process);
        if (status != HL_STATUS_OK) {
            hl_production_result_release(host, (hl_host_handle)(uintptr_t)result);
            return status;
        }
        spawned = (hl_host_result){HL_STATUS_OK, 0, *process, 0};
    }
    if (spawned.status != HL_STATUS_OK) {
        hl_production_result_release(host, (hl_host_handle)(uintptr_t)result);
        return (hl_status)spawned.status;
    }
    if (*process == HL_HOST_HANDLE_INVALID) *process = spawned.value;
    *result_token = (hl_host_handle)(uintptr_t)result;
    return HL_STATUS_OK;
}

static hl_status hl_production_finish_process(const hl_host_services *host, hl_host_handle token,
                                              const hl_host_result *waited, hl_engine_exit *result) {
    hl_production_result_state *state = (hl_production_result_state *)(uintptr_t)token;
    hl_engine_child_result record;
    if (waited->detail == HL_HOST_PROCESS_EXIT_SIGNAL) {
        hl_production_result_release(host, token);
        result->kind = HL_ENGINE_EXIT_SIGNAL;
        result->guest_status = (int32_t)waited->value;
        result->detail = 0;
        return HL_STATUS_OK;
    }
    memset(&record, 0, sizeof(record));
    if (state != NULL &&
        atomic_load_explicit((_Atomic uint32_t *)&state->record->magic, memory_order_acquire) ==
            HL_ENGINE_CHILD_RESULT_MAGIC)
        memcpy(&record, state->record, sizeof(record));
    hl_production_result_release(host, token);
    result->kind = HL_ENGINE_EXIT_ENGINE_ERROR;
    result->guest_status = HL_STATUS_CORRUPT;
    result->detail = record.magic == HL_ENGINE_CHILD_RESULT_MAGIC ? sizeof(record) : 0;
    if (record.magic != HL_ENGINE_CHILD_RESULT_MAGIC ||
        record.version != HL_ENGINE_CHILD_RESULT_VERSION || waited->detail != HL_HOST_PROCESS_EXIT_CODE) {
        fprintf(stderr,
                "hl-engine: invalid child result magic=%llx version=%u wait-kind=%llu wait-value=%llu\n",
                (unsigned long long)record.magic, record.version, (unsigned long long)waited->detail,
                (unsigned long long)waited->value);
        return HL_STATUS_CORRUPT;
    }
    if (record.engine_status != HL_STATUS_OK) {
        if (record.engine_status < HL_STATUS_INVALID_ARGUMENT || record.engine_status > HL_STATUS_ADDRESS_IN_USE)
            return HL_STATUS_CORRUPT;
        result->guest_status = record.engine_status;
        result->detail = record.detail;
        return (hl_status)record.engine_status;
    }
    if ((uint32_t)record.guest_status > 255u || waited->value != (uint32_t)record.guest_status) {
        fprintf(stderr, "hl-engine: child result mismatch guest=%d wait=%llu engine=%d\n", record.guest_status,
                (unsigned long long)waited->value, record.engine_status);
        return HL_STATUS_CORRUPT;
    }
    result->kind = HL_ENGINE_EXIT_CODE;
    result->guest_status = record.guest_status;
    result->detail = 0;
    return HL_STATUS_OK;
}

static const hl_engine_backend backend = {HL_PRODUCTION_GUEST_ISA, hl_production_start_process,
                                          hl_production_finish_process, hl_production_result_release};

void hl_target_register_backend(void) {
    hl_engine_backend_register(&backend);
}

#ifndef HL_EMBEDDED_BUILD
__attribute__((constructor)) static void hl_production_register_backend(void) {
    hl_target_register_backend();
}
#endif
