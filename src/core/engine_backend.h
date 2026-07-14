#ifndef HL_ENGINE_BACKEND_H
#define HL_ENGINE_BACKEND_H

#include "hl/engine.h"

typedef struct hl_engine_backend {
    uint32_t guest_isa;
    hl_status (*run_process)(const char *rootfs, int argc, const char *const argv[], hl_engine_exit *result);
} hl_engine_backend;

void hl_engine_backend_register(const hl_engine_backend *backend);

#endif
