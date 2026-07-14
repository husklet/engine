#include "hl/engine.h"
#include "engine_backend.h"

#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

static const hl_engine_backend *production_backend;

void hl_engine_backend_register(const hl_engine_backend *backend) {
    production_backend = backend;
}

struct hl_engine {
    hl_engine_config config;
    hl_host_services host;
    const hl_engine_backend *backend;
    atomic_flag lock;
    hl_host_handle process;
    uint32_t state;
    uint32_t pending_termination;
};

enum { HL_ENGINE_CREATED = 0, HL_ENGINE_STARTING = 1, HL_ENGINE_RUNNING = 2, HL_ENGINE_FINISHED = 3 };

static void hl_engine_lock(hl_engine *engine) {
    while (atomic_flag_test_and_set_explicit(&engine->lock, memory_order_acquire)) {}
}

static void hl_engine_unlock(hl_engine *engine) {
    atomic_flag_clear_explicit(&engine->lock, memory_order_release);
}

uint32_t hl_engine_abi(void) {
    return HL_ENGINE_ABI;
}

const char *hl_engine_version(void) {
    return "0.1.0";
}

hl_status hl_engine_create(const hl_engine_config *config, const hl_host_services *host, hl_engine **out_engine) {
    hl_engine *engine;
    hl_status status;
    if (config == NULL || host == NULL || out_engine == NULL) return HL_STATUS_INVALID_ARGUMENT;
    *out_engine = NULL;
    if (config->abi != HL_ENGINE_ABI || config->size < sizeof(*config)) return HL_STATUS_ABI_MISMATCH;
    if (config->guest_isa != HL_GUEST_ISA_AARCH64 && config->guest_isa != HL_GUEST_ISA_X86_64)
        return HL_STATUS_INVALID_ARGUMENT;
    if (config->payload_size != 0 && config->payload == NULL) return HL_STATUS_INVALID_ARGUMENT;
    status = hl_host_services_validate(host, HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK);
    if (status != HL_STATUS_OK) return status;
    engine = calloc(1, sizeof(*engine));
    if (engine == NULL) return HL_STATUS_OUT_OF_MEMORY;
    memcpy(&engine->config, config, sizeof(*config));
    memcpy(&engine->host, host, sizeof(*host));
    atomic_flag_clear(&engine->lock);
    engine->backend = production_backend;
    *out_engine = engine;
    return HL_STATUS_OK;
}

hl_status hl_engine_run(hl_engine *engine, int argc, const char *const argv[], hl_engine_exit *out_exit) {
    hl_host_result waited;
    hl_host_result closed;
    hl_host_handle process = HL_HOST_HANDLE_INVALID;
    uint32_t pending;
    hl_status status;
    if (engine == NULL || argc < 0 || (argc != 0 && argv == NULL) || out_exit == NULL)
        return HL_STATUS_INVALID_ARGUMENT;
    if (out_exit->abi != HL_ENGINE_ABI || out_exit->size < sizeof(*out_exit)) return HL_STATUS_ABI_MISMATCH;
    hl_engine_lock(engine);
    if (engine->state != HL_ENGINE_CREATED) {
        hl_engine_unlock(engine);
        return HL_STATUS_BUSY;
    }
    engine->state = HL_ENGINE_STARTING;
    hl_engine_unlock(engine);
    out_exit->kind = HL_ENGINE_EXIT_ENGINE_ERROR;
    out_exit->guest_status = HL_STATUS_NOT_SUPPORTED;
    out_exit->detail = engine->config.guest_isa;
    if (engine->backend == NULL || engine->backend->guest_isa != engine->config.guest_isa ||
        engine->backend->start_process == NULL) {
        hl_engine_lock(engine);
        engine->state = HL_ENGINE_FINISHED;
        hl_engine_unlock(engine);
        return HL_STATUS_NOT_SUPPORTED;
    }
    status = engine->backend->start_process(&engine->host, engine->config.rootfs, argc, argv, &process);
    if (status != HL_STATUS_OK) {
        hl_engine_lock(engine);
        engine->state = HL_ENGINE_FINISHED;
        hl_engine_unlock(engine);
        return status;
    }
    hl_engine_lock(engine);
    engine->process = process;
    engine->state = HL_ENGINE_RUNNING;
    pending = engine->pending_termination;
    hl_engine_unlock(engine);
    if (pending != 0) engine->host.process->terminate(engine->host.context, process, pending);
    waited = engine->host.process->wait(engine->host.context, process, HL_HOST_DEADLINE_INFINITE);
    hl_engine_lock(engine);
    engine->process = HL_HOST_HANDLE_INVALID;
    engine->state = HL_ENGINE_FINISHED;
    hl_engine_unlock(engine);
    closed = engine->host.process->close(engine->host.context, process);
    if (waited.status != HL_STATUS_OK) return (hl_status)waited.status;
    if (closed.status != HL_STATUS_OK) return (hl_status)closed.status;
    out_exit->detail = 0;
    if (waited.detail == HL_HOST_PROCESS_EXIT_CODE) {
        out_exit->kind = HL_ENGINE_EXIT_CODE;
        out_exit->guest_status = (int32_t)waited.value;
    } else if (waited.detail == HL_HOST_PROCESS_EXIT_SIGNAL) {
        out_exit->kind = HL_ENGINE_EXIT_SIGNAL;
        out_exit->guest_status = (int32_t)waited.value;
    } else {
        out_exit->guest_status = HL_STATUS_CORRUPT;
        return HL_STATUS_CORRUPT;
    }
    return HL_STATUS_OK;
}

hl_status hl_engine_request(hl_engine *engine, uint32_t request, const void *data, size_t data_size) {
    uint32_t reason;
    hl_host_handle process;
    hl_status status;
    if (engine == NULL || (data_size != 0 && data == NULL)) return HL_STATUS_INVALID_ARGUMENT;
    if (data_size != 0) return HL_STATUS_INVALID_ARGUMENT;
    if (request == HL_ENGINE_REQUEST_INTERRUPT)
        reason = HL_HOST_PROCESS_TERMINATE_INTERRUPT;
    else if (request == HL_ENGINE_REQUEST_FORCE_STOP)
        reason = HL_HOST_PROCESS_TERMINATE_FORCE;
    else
        return HL_STATUS_NOT_SUPPORTED;
    hl_engine_lock(engine);
    if (engine->state == HL_ENGINE_CREATED || engine->state == HL_ENGINE_FINISHED) {
        hl_engine_unlock(engine);
        return HL_STATUS_BUSY;
    }
    engine->pending_termination = reason;
    process = engine->process;
    hl_engine_unlock(engine);
    if (process == HL_HOST_HANDLE_INVALID) return HL_STATUS_OK;
    status = (hl_status)engine->host.process->terminate(engine->host.context, process, reason).status;
    if (status == HL_STATUS_INVALID_ARGUMENT) {
        hl_engine_lock(engine);
        if (engine->state == HL_ENGINE_FINISHED) status = HL_STATUS_BUSY;
        hl_engine_unlock(engine);
    }
    return status;
}

void hl_engine_destroy(hl_engine *engine) {
    free(engine);
}
