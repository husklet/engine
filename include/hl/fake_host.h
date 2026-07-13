#ifndef HL_FAKE_HOST_H
#define HL_FAKE_HOST_H

#include "hl/host_services.h"

HL_EXTERN_C_BEGIN

typedef struct hl_fake_host {
    uint64_t monotonic_ns;
    uint64_t realtime_ns;
    uint64_t next_handle;
    uint32_t live_mappings;
    hl_status next_failure;
} hl_fake_host;

HL_API void hl_fake_host_init(hl_fake_host *fake, hl_host_services *services);
HL_API void hl_fake_host_fail_next(hl_fake_host *fake, hl_status status);

HL_EXTERN_C_END

#endif
