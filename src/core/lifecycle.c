#include "target/namespace.h"
#include "engine_backend.h"
#include "engine_result.h"
#include "options.h"

#include <stdatomic.h>
#include <signal.h>
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
static _Atomic int result_published;

static int hl_engine_child_result_claim(void) {
    int expected = 0;
    if (active_result == NULL) return 0;
    if (atomic_compare_exchange_strong_explicit(&result_published, &expected, 1,
                                                memory_order_acq_rel, memory_order_acquire))
        return 1;
    while (atomic_load_explicit(&result_published, memory_order_acquire) == 1) {}
    return 0;
}

void hl_engine_child_result_publish(int32_t guest_status, hl_status engine_status, uint64_t detail) {
    hl_engine_child_result record = {0, HL_ENGINE_CHILD_RESULT_VERSION, guest_status, engine_status,
                                     HL_ENGINE_CHILD_RESULT_EXIT, 0, detail};
    if (!hl_engine_child_result_claim()) return;
    memcpy(active_result, &record, sizeof(record));
    atomic_store_explicit((_Atomic uint32_t *)&active_result->magic, HL_ENGINE_CHILD_RESULT_MAGIC,
                          memory_order_release);
    atomic_store_explicit(&result_published, 2, memory_order_release);
}

void hl_engine_child_result_publish_signal(int32_t guest_signal) {
    if (!hl_engine_child_result_claim()) return;
    active_result->version = HL_ENGINE_CHILD_RESULT_VERSION;
    active_result->guest_status = guest_signal;
    active_result->engine_status = HL_STATUS_OK;
    active_result->kind = HL_ENGINE_CHILD_RESULT_SIGNAL;
    active_result->reserved = 0;
    active_result->detail = 0;
    atomic_store_explicit((_Atomic uint32_t *)&active_result->magic, HL_ENGINE_CHILD_RESULT_MAGIC,
                          memory_order_release);
    atomic_store_explicit(&result_published, 2, memory_order_release);
}

void hl_engine_child_result_after_fork(void) {
    active_result = NULL;
    atomic_store_explicit(&result_published, 2, memory_order_release);
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
    atomic_store_explicit(&result_published, 0, memory_order_release);
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
#if defined(__APPLE__)
        // A translated bad-address fault may reach waitpid as the raw macOS SIGBUS when the kernel does
        // not enter the POSIX guard. Linux reports that access as SIGSEGV. Genuine guest file-EOF SIGBUS
        // is published through hl_engine_child_result and therefore takes the record path below.
        result->guest_status = waited->value == SIGBUS ? 11 : (int32_t)waited->value;
#else
        result->guest_status = (int32_t)waited->value;
#endif
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
    if (record.kind == HL_ENGINE_CHILD_RESULT_SIGNAL) {
        if (record.guest_status < 1 || record.guest_status > 64 ||
            waited->value != (uint32_t)(128 + record.guest_status))
            return HL_STATUS_CORRUPT;
        result->kind = HL_ENGINE_EXIT_SIGNAL;
        result->guest_status = record.guest_status;
        result->detail = 0;
        return HL_STATUS_OK;
    }
    if (record.kind != HL_ENGINE_CHILD_RESULT_EXIT || (uint32_t)record.guest_status > 255u ||
        waited->value != (uint32_t)record.guest_status) {
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
