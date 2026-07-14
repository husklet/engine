#include "test.h"

#include "../../src/host/clock.h"
#include "hl/fake.h"

#include <errno.h>

int main(void) {
    hl_fake_host fake;
    hl_host_services services;
    struct timespec value;
    uint64_t nanoseconds;

    hl_fake_host_init(&fake, &services);
    fake.monotonic_ns = UINT64_C(3000000007);
    fake.realtime_ns = UINT64_C(9000000011);
    HL_CHECK(hl_production_clock_gettime(&services, HL_PRODUCTION_CLOCK_MONOTONIC, &value) == 0);
    HL_CHECK(value.tv_sec == 3 && value.tv_nsec == 7);
    HL_CHECK(hl_production_clock_gettime(&services, HL_PRODUCTION_CLOCK_REALTIME, &value) == 0);
    HL_CHECK(value.tv_sec == 9 && value.tv_nsec == 11);
    fake.monotonic_ns = UINT64_C(123456789);
    HL_CHECK(hl_production_clock_nanoseconds(&services, HL_PRODUCTION_CLOCK_MONOTONIC, &nanoseconds) == 0);
    HL_CHECK(nanoseconds == UINT64_C(123456789));
    errno = 0;
    HL_CHECK(hl_production_clock_nanoseconds(&services, 99, &nanoseconds) == -1 && errno == EINVAL);
    hl_fake_host_fail_next(&fake, HL_STATUS_PLATFORM_FAILURE);
    errno = 0;
    HL_CHECK(hl_production_clock_gettime(&services, HL_PRODUCTION_CLOCK_MONOTONIC, &value) == -1 && errno == EIO);
    errno = 0;
    HL_CHECK(hl_production_clock_gettime(NULL, HL_PRODUCTION_CLOCK_MONOTONIC, &value) == -1 && errno == EINVAL);
    return EXIT_SUCCESS;
}
