#ifndef HL_FAKE_H
#define HL_FAKE_H

#include "hl/host_services.h"

HL_EXTERN_C_BEGIN

typedef struct hl_fake_host {
    uint64_t monotonic_ns;
    uint64_t realtime_ns;
    uint64_t next_handle;
    uint32_t live_mappings;
    uint32_t live_processes;
    uint32_t process_waited;
    uint32_t process_block_wait;
    uint32_t process_exit_kind;
    int32_t process_exit_value;
    hl_status next_failure;
} hl_fake_host;

HL_API void hl_fake_host_init(hl_fake_host *fake, hl_host_services *services);
HL_API void hl_fake_host_fail_next(hl_fake_host *fake, hl_status status);
HL_API void hl_fake_host_block_process_wait(hl_fake_host *fake, uint32_t block);

HL_EXTERN_C_END

#endif
