#include "hl/engine.h"
#include "hl/linux_abi.h"
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
    hl_linux_abi box;
    hl_linux_fd_entry *box_fds;
    hl_linux_ofd_entry *box_ofds;
    uint32_t box_initialized;
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
    hl_host_handle *candidate_handles = NULL;
    hl_status status;
    if (config == NULL || host == NULL || out_engine == NULL) return HL_STATUS_INVALID_ARGUMENT;
    *out_engine = NULL;
    if (config->abi != HL_ENGINE_ABI || config->size < sizeof(*config)) return HL_STATUS_ABI_MISMATCH;
    if (config->guest_isa != HL_GUEST_ISA_AARCH64 && config->guest_isa != HL_GUEST_ISA_X86_64)
        return HL_STATUS_INVALID_ARGUMENT;
    if (config->payload_size != 0 && config->payload == NULL) return HL_STATUS_INVALID_ARGUMENT;
    if (config->fd_binding_count != 0 && config->fd_bindings == NULL) return HL_STATUS_INVALID_ARGUMENT;
    status = hl_host_services_validate(host, HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK);
    if (status != HL_STATUS_OK) return status;
    engine = calloc(1, sizeof(*engine));
    if (engine == NULL) return HL_STATUS_OUT_OF_MEMORY;
    memcpy(&engine->config, config, sizeof(*config));
    memcpy(&engine->host, host, sizeof(*host));
    if (config->fd_binding_count != 0) {
        uint32_t index;
        uint32_t fd_capacity = HL_LINUX_FD_LIMIT;
        status = hl_host_services_validate(host, HL_HOST_CAP_FILE | HL_HOST_CAP_SYNC);
        if (status != HL_STATUS_OK) goto fail;
        for (index = 0; index < config->fd_binding_count; ++index) {
            const hl_engine_fd_binding *binding = &config->fd_bindings[index];
            uint32_t previous;
            if (binding->abi != HL_ENGINE_ABI || binding->size < sizeof(*binding) ||
                binding->host_handle == HL_HOST_HANDLE_INVALID || binding->guest_fd >= HL_LINUX_FD_LIMIT ||
                (binding->ownership != HL_ENGINE_FD_TRANSFER && binding->ownership != HL_ENGINE_FD_BORROW)) {
                status = HL_STATUS_INVALID_ARGUMENT;
                goto fail;
            }
            for (previous = 0; previous < index; ++previous) {
                if (config->fd_bindings[previous].guest_fd == binding->guest_fd) {
                    status = HL_STATUS_INVALID_ARGUMENT;
                    goto fail;
                }
            }
        }
        engine->box_fds = calloc(fd_capacity, sizeof(*engine->box_fds));
        engine->box_ofds = calloc(HL_LINUX_OFD_LIMIT, sizeof(*engine->box_ofds));
        if (engine->box_fds == NULL || engine->box_ofds == NULL) {
            status = HL_STATUS_OUT_OF_MEMORY;
            goto fail;
        }
        candidate_handles = malloc(config->fd_binding_count * sizeof(*candidate_handles));
        if (candidate_handles == NULL) {
            status = HL_STATUS_OUT_OF_MEMORY;
            goto fail;
        }
        for (index = 0; index < config->fd_binding_count; ++index)
            candidate_handles[index] = HL_HOST_HANDLE_INVALID;
        status = hl_linux_abi_init(&engine->box, &engine->host, engine->box_fds, fd_capacity, engine->box_ofds,
                                   HL_LINUX_OFD_LIMIT);
        if (status != HL_STATUS_OK) goto fail;
        engine->box_initialized = 1;
        for (index = 0; index < config->fd_binding_count; ++index) {
            const hl_engine_fd_binding *binding = &config->fd_bindings[index];
            hl_host_result cloned = engine->host.file->clone_for_fork(engine->host.context, binding->host_handle);
            if (cloned.status != HL_STATUS_OK || cloned.value == HL_HOST_HANDLE_INVALID) {
                status = cloned.status == HL_STATUS_OK ? HL_STATUS_PLATFORM_FAILURE : (hl_status)cloned.status;
                goto fail;
            }
            candidate_handles[index] = cloned.value;
            status = hl_linux_fd_install_at(&engine->box, binding->guest_fd, candidate_handles[index],
                                            binding->status_flags, binding->descriptor_flags);
            if (status != HL_STATUS_OK) goto fail;
            candidate_handles[index] = HL_HOST_HANDLE_INVALID;
        }
        for (index = 0; index < config->fd_binding_count; ++index) {
            const hl_engine_fd_binding *binding = &config->fd_bindings[index];
            if (binding->ownership == HL_ENGINE_FD_TRANSFER)
                (void)engine->host.file->close(engine->host.context, binding->host_handle);
        }
        engine->config.fd_bindings = NULL;
        engine->config.fd_binding_count = 0;
    }
    atomic_flag_clear(&engine->lock);
    engine->backend = production_backend;
    free(candidate_handles);
    *out_engine = engine;
    return HL_STATUS_OK;
fail:
    if (candidate_handles != NULL && engine != NULL) {
        uint32_t index;
        for (index = 0; index < config->fd_binding_count; ++index) {
            if (candidate_handles[index] != HL_HOST_HANDLE_INVALID)
                (void)engine->host.file->close(engine->host.context, candidate_handles[index]);
        }
    }
    free(candidate_handles);
    if (engine != NULL) {
        uint32_t fd;
        if (engine->box_initialized) {
            for (fd = 0; fd < engine->box.fd_capacity; ++fd) {
                hl_host_handle handle;
                if (hl_linux_fd_close(&engine->box, fd, &handle) == HL_STATUS_OK && handle != HL_HOST_HANDLE_INVALID)
                    (void)engine->host.file->close(engine->host.context, handle);
            }
            (void)hl_linux_abi_destroy(&engine->box);
        }
        free(engine->box_fds);
        free(engine->box_ofds);
        free(engine);
    }
    return status;
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
    status = engine->backend->start_process(&engine->host, engine->box_initialized ? &engine->box : NULL,
                                            engine->config.rootfs, (uint32_t)argc, argv, &process);
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
    uint32_t fd;
    if (engine == NULL) return;
    if (engine->box_initialized) {
        for (fd = 0; fd < engine->box.fd_capacity; ++fd)
            (void)hl_linux_close(&engine->box, fd);
        (void)hl_linux_abi_destroy(&engine->box);
    }
    free(engine->box_fds);
    free(engine->box_ofds);
    free(engine);
}
