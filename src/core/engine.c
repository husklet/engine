#include "hl/engine.h"
#include "engine_backend.h"

#include <stdlib.h>
#include <string.h>

static const hl_engine_backend *production_backend;

void hl_engine_backend_register(const hl_engine_backend *backend) {
    production_backend = backend;
}

struct hl_engine {
    hl_engine_config config;
    hl_host_services host;
    const hl_engine_backend *backend;
    int has_run;
};

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
    engine->backend = production_backend;
    *out_engine = engine;
    return HL_STATUS_OK;
}

hl_status hl_engine_run(hl_engine *engine, int argc, const char *const argv[], hl_engine_exit *out_exit) {
    if (engine == NULL || argc < 0 || (argc != 0 && argv == NULL) || out_exit == NULL)
        return HL_STATUS_INVALID_ARGUMENT;
    if (out_exit->abi != HL_ENGINE_ABI || out_exit->size < sizeof(*out_exit)) return HL_STATUS_ABI_MISMATCH;
    if (engine->has_run) return HL_STATUS_BUSY;
    out_exit->kind = HL_ENGINE_EXIT_ENGINE_ERROR;
    out_exit->guest_status = HL_STATUS_NOT_SUPPORTED;
    out_exit->detail = engine->config.guest_isa;
    if (engine->backend == NULL || engine->backend->guest_isa != engine->config.guest_isa ||
        engine->backend->run_process == NULL)
        return HL_STATUS_NOT_SUPPORTED;
    engine->has_run = 1;
    return engine->backend->run_process(&engine->host, engine->config.rootfs, argc, argv, out_exit);
}

hl_status hl_engine_request(hl_engine *engine, uint32_t request, const void *data, size_t data_size) {
    (void)request;
    if (engine == NULL || (data_size != 0 && data == NULL)) return HL_STATUS_INVALID_ARGUMENT;
    return HL_STATUS_NOT_SUPPORTED;
}

void hl_engine_destroy(hl_engine *engine) {
    free(engine);
}
