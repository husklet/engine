#ifndef HL_CORE_TARGET_NATIVE_H
#define HL_CORE_TARGET_NATIVE_H

#include "hl/host_services.h"

#if defined(__APPLE__)
#include "hl/macos.h"
typedef hl_host_macos hl_native_host;
#define HL_NATIVE_HOST_NAME "macos"

static inline hl_status hl_native_host_create(hl_native_host **host, hl_host_services *services) {
    return hl_host_macos_create(host, services);
}

static inline void hl_native_host_destroy(hl_native_host *host) {
    hl_host_macos_destroy(host);
}
#elif defined(__linux__)
#include "hl/linux.h"
typedef hl_host_linux hl_native_host;
#define HL_NATIVE_HOST_NAME "linux"

static inline hl_status hl_native_host_create(hl_native_host **host, hl_host_services *services) {
    return hl_host_linux_create(host, services);
}

static inline void hl_native_host_destroy(hl_native_host *host) {
    hl_host_linux_destroy(host);
}
#else
#error "hl engine has no native host backend for this platform"
#endif

/* Bind once per production process. An injected host must remain identical on every later launch. */
static inline int hl_native_host_bind(hl_native_host **native, hl_host_services *bound,
                                      const hl_host_services *injected) {
    if (native == NULL || bound == NULL) return -1;
    if (bound->abi != 0) {
        if (injected != NULL)
            return injected->context == bound->context && injected->memory == bound->memory &&
                           injected->clock == bound->clock
                       ? 0
                       : -1;
        return *native != NULL ? 0 : -1;
    }
    if (injected != NULL)
        *bound = *injected;
    else if (hl_native_host_create(native, bound) != HL_STATUS_OK)
        return -1;
    return hl_host_services_validate(bound, HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK | HL_HOST_CAP_CODE_MAPPING) ==
                   HL_STATUS_OK
               ? 0
               : -1;
}

#endif
