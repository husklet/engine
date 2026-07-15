#include "target/namespace.h"
#include "engine_backend.h"
#include "engine_result.h"
#include "options.h"

#include <stdio.h>

#ifndef HL_PRODUCTION_GUEST_ISA
#error HL_PRODUCTION_GUEST_ISA is required
#endif

int hl_run_linux_guest(const hl_host_services *host, hl_linux_abi *box, const char *rootfs, uint32_t argc,
                       char *const argv[]);
hl_status hl_run_linux_guest_status(void);

static const hl_host_services *result_host;
static hl_host_handle active_result_read;
static hl_host_handle active_result_write;
static int result_published;

void hl_engine_child_result_publish(int32_t guest_status, hl_status engine_status, uint64_t detail) {
    hl_engine_child_result record = {HL_ENGINE_CHILD_RESULT_MAGIC, HL_ENGINE_CHILD_RESULT_VERSION, guest_status,
                                     engine_status, detail};
    size_t offset = 0;
    if (result_published || result_host == NULL) return;
    result_published = 1;
    while (offset < sizeof(record)) {
        hl_host_result written = result_host->stream->write(
            result_host->context, active_result_write,
            (hl_host_const_bytes){(const unsigned char *)&record + offset, sizeof(record) - offset});
        if (written.status != HL_STATUS_OK || written.value == 0 || written.value > sizeof(record) - offset) break;
        offset += (size_t)written.value;
    }
}

void hl_engine_child_result_after_fork(void) {
    if (result_host == NULL) return;
    (void)result_host->stream->close(result_host->context, active_result_read);
    (void)result_host->stream->close(result_host->context, active_result_write);
    result_host = NULL;
    result_published = 1;
}

typedef struct hl_production_entry_context {
    const hl_engine_config *config;
    uint32_t argc;
    const char *const *argv;
    const hl_host_services *host;
    hl_linux_abi *box;
    hl_options *options;
    hl_host_handle result_read;
    hl_host_handle result_write;
} hl_production_entry_context;

static int32_t hl_production_entry(void *opaque) {
    hl_production_entry_context *context = opaque;
    result_host = context->host;
    active_result_read = context->result_read;
    active_result_write = context->result_write;
    result_published = 0;
    hl_options *previous = hl_options_bind_process(context->options);
    int32_t result = hl_run_linux_guest(context->host, context->box, context->config->rootfs, context->argc,
                                        (char *const *)(uintptr_t)context->argv);
    (void)hl_options_bind_process(previous);
    hl_engine_child_result_publish(result, hl_run_linux_guest_status(), 0);
    (void)result_host->stream->close(result_host->context, active_result_write);
    return result;
}

static hl_status hl_production_start_process(const hl_host_services *host, hl_linux_abi *box,
                                             hl_options *options,
                                             const hl_engine_config *config, uint32_t argc,
                                             const char *const argv[], hl_host_handle *process,
                                             hl_host_handle *result_stream) {
    hl_production_entry_context entry = {0};
    hl_host_result spawned;
    hl_host_result pipe;
    if (hl_host_services_validate(host, HL_HOST_CAP_PROCESS | HL_HOST_CAP_STREAM) != HL_STATUS_OK)
        return HL_STATUS_NOT_SUPPORTED;
    *process = HL_HOST_HANDLE_INVALID;
    *result_stream = HL_HOST_HANDLE_INVALID;
    pipe = host->stream->pipe_pair(host->context, HL_HOST_STREAM_NONBLOCK);
    if (pipe.status != HL_STATUS_OK) return (hl_status)pipe.status;
    entry.config = config;
    entry.argc = argc;
    entry.argv = argv;
    entry.host = host;
    entry.box = box;
    entry.options = options;
    entry.result_read = pipe.value;
    entry.result_write = pipe.detail;
    if (box == NULL) {
        spawned = host->process->spawn_cloned(host->context, hl_production_entry, &entry);
    } else {
        hl_status status = hl_linux_abi_spawn(box, hl_production_entry, &entry, process);
        if (status != HL_STATUS_OK) {
            (void)host->stream->close(host->context, pipe.value);
            (void)host->stream->close(host->context, pipe.detail);
            return status;
        }
        spawned = (hl_host_result){HL_STATUS_OK, 0, *process, 0};
    }
    if (spawned.status != HL_STATUS_OK) {
        (void)host->stream->close(host->context, pipe.value);
        (void)host->stream->close(host->context, pipe.detail);
        return (hl_status)spawned.status;
    }
    if (*process == HL_HOST_HANDLE_INVALID) *process = spawned.value;
    (void)host->stream->close(host->context, pipe.detail);
    *result_stream = pipe.value;
    return HL_STATUS_OK;
}

static hl_status hl_production_finish_process(const hl_host_services *host, hl_host_handle stream,
                                              const hl_host_result *waited, hl_engine_exit *result) {
    hl_engine_child_result record;
    size_t offset = 0;
    if (waited->detail == HL_HOST_PROCESS_EXIT_SIGNAL) {
        (void)host->stream->close(host->context, stream);
        result->kind = HL_ENGINE_EXIT_SIGNAL;
        result->guest_status = (int32_t)waited->value;
        result->detail = 0;
        return HL_STATUS_OK;
    }
    while (offset < sizeof(record)) {
        hl_host_result read = host->stream->read(host->context, stream,
                                                 (hl_host_bytes){(unsigned char *)&record + offset,
                                                                 sizeof(record) - offset});
        if (read.status != HL_STATUS_OK || read.value == 0 || read.value > sizeof(record) - offset) break;
        offset += (size_t)read.value;
    }
    (void)host->stream->close(host->context, stream);
    result->kind = HL_ENGINE_EXIT_ENGINE_ERROR;
    result->guest_status = HL_STATUS_CORRUPT;
    result->detail = offset;
    if (offset != sizeof(record) || record.magic != HL_ENGINE_CHILD_RESULT_MAGIC ||
        record.version != HL_ENGINE_CHILD_RESULT_VERSION || waited->detail != HL_HOST_PROCESS_EXIT_CODE)
        return HL_STATUS_CORRUPT;
    if (record.engine_status != HL_STATUS_OK) {
        if (record.engine_status < HL_STATUS_INVALID_ARGUMENT || record.engine_status > HL_STATUS_ADDRESS_IN_USE)
            return HL_STATUS_CORRUPT;
        result->guest_status = record.engine_status;
        result->detail = record.detail;
        return (hl_status)record.engine_status;
    }
    if ((uint32_t)record.guest_status > 255u || waited->value != (uint32_t)record.guest_status)
        return HL_STATUS_CORRUPT;
    result->kind = HL_ENGINE_EXIT_CODE;
    result->guest_status = record.guest_status;
    result->detail = 0;
    return HL_STATUS_OK;
}

static const hl_engine_backend backend = {HL_PRODUCTION_GUEST_ISA, hl_production_start_process,
                                          hl_production_finish_process};

__attribute__((constructor)) static void hl_production_register_backend(void) {
    hl_engine_backend_register(&backend);
}
