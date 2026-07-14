#include "clock.h"

#include <errno.h>

int hl_production_clock_gettime(const hl_host_services *services, int clock_id, struct timespec *output) {
    hl_host_result result;
    if (services == NULL || output == NULL || services->clock == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (clock_id == HL_PRODUCTION_CLOCK_MONOTONIC)
        result = services->clock->monotonic_ns(services->context);
    else if (clock_id == HL_PRODUCTION_CLOCK_REALTIME)
        result = services->clock->realtime_ns(services->context);
    else {
        errno = EINVAL;
        return -1;
    }
    if (result.status != HL_STATUS_OK) {
        errno = result.status == HL_STATUS_INVALID_ARGUMENT ? EINVAL : EIO;
        return -1;
    }
    output->tv_sec = (time_t)(result.value / UINT64_C(1000000000));
    output->tv_nsec = (long)(result.value % UINT64_C(1000000000));
    return 0;
}
