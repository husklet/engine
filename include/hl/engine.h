#ifndef HL_ENGINE_H
#define HL_ENGINE_H

#include "hl/base.h"
#include "hl/host_services.h"

HL_EXTERN_C_BEGIN

#define HL_ENGINE_ABI 2u

typedef struct hl_engine hl_engine;

typedef enum hl_guest_isa { HL_GUEST_ISA_AARCH64 = 1, HL_GUEST_ISA_X86_64 = 2 } hl_guest_isa;

typedef enum hl_engine_exit_kind {
    HL_ENGINE_EXIT_NONE = 0,
    HL_ENGINE_EXIT_CODE = 1,
    HL_ENGINE_EXIT_SIGNAL = 2,
    HL_ENGINE_EXIT_FAULT = 3,
    HL_ENGINE_EXIT_ENGINE_ERROR = 4
} hl_engine_exit_kind;

typedef enum hl_engine_request_kind {
    HL_ENGINE_REQUEST_INTERRUPT = 1,
    HL_ENGINE_REQUEST_FORCE_STOP = 2
} hl_engine_request_kind;

typedef struct hl_engine_config {
    HL_ABI_HEADER;
    uint32_t guest_isa;
    uint32_t flags;
    uint64_t memory_limit;
    uint32_t pid_limit;
    uint32_t cpu_limit;
    /* Optional opaque program image/state owned by the caller for the engine lifetime. */
    const void *payload;
    size_t payload_size;
    /* Optional Linux root filesystem path owned by the caller for the engine lifetime. */
    const char *rootfs;
} hl_engine_config;

typedef struct hl_engine_exit {
    HL_ABI_HEADER;
    uint32_t kind;
    int32_t guest_status;
    uint64_t detail;
} hl_engine_exit;

HL_API uint32_t hl_engine_abi(void);
HL_API const char *hl_engine_version(void);
HL_API hl_status hl_engine_create(const hl_engine_config *config, const hl_host_services *host, hl_engine **out_engine);
HL_API hl_status hl_engine_run(hl_engine *engine, int argc, const char *const argv[], hl_engine_exit *out_exit);
HL_API hl_status hl_engine_request(hl_engine *engine, uint32_t request, const void *data, size_t data_size);
HL_API void hl_engine_destroy(hl_engine *engine);

HL_EXTERN_C_END

#endif
