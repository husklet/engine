#ifndef HL_ENGINE_H
#define HL_ENGINE_H

#include "hl/base.h"
#include "hl/host_services.h"

HL_EXTERN_C_BEGIN

#define HL_ENGINE_ABI 3u

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

typedef enum hl_engine_fd_ownership {
    /* Ownership transfers only after hl_engine_create succeeds. */
    HL_ENGINE_FD_TRANSFER = 1,
    /* Engine clones the handle and never closes the caller's original. */
    HL_ENGINE_FD_BORROW = 2
} hl_engine_fd_ownership;

typedef struct hl_engine_fd_binding {
    HL_ABI_HEADER;
    uint32_t guest_fd;
    uint32_t status_flags;
    uint32_t descriptor_flags;
    uint32_t ownership;
    hl_host_handle host_handle;
} hl_engine_fd_binding;

typedef struct hl_engine_config {
    HL_ABI_HEADER;
    uint32_t guest_isa;
    /* Must be zero. No public engine flags are currently defined. */
    uint32_t flags;
    uint64_t memory_limit;
    uint32_t pid_limit;
    uint32_t cpu_limit;
    /* Reserved for a future executable-image API. Nonempty payloads are not currently supported. */
    const void *payload;
    size_t payload_size;
    /* Optional Linux root filesystem path owned by the caller for the engine lifetime. */
    const char *rootfs;
    const hl_engine_fd_binding *fd_bindings;
    uint32_t fd_binding_count;
    /* Must be zero. */
    uint32_t reserved;
} hl_engine_config;

typedef struct hl_engine_exit {
    HL_ABI_HEADER;
    uint32_t kind;
    int32_t guest_status;
    uint64_t detail;
} hl_engine_exit;

HL_API uint32_t hl_engine_abi(void);
HL_API const char *hl_engine_version(void);
/* host, its callback-group tables, its context, and config-owned pointers remain valid until destroy. */
HL_API hl_status hl_engine_create(const hl_engine_config *config, const hl_host_services *host, hl_engine **out_engine);
HL_API hl_status hl_engine_run(hl_engine *engine, int argc, const char *const argv[], hl_engine_exit *out_exit);
HL_API hl_status hl_engine_request(hl_engine *engine, uint32_t request, const void *data, size_t data_size);
HL_API void hl_engine_destroy(hl_engine *engine);

HL_EXTERN_C_END

#endif
