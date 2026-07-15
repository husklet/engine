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
    fake.raw_monotonic_ns = UINT64_C(4000000013);
    fake.architectural_counter_hz = UINT64_C(24000000);
    fake.process_cpu_ns = UINT64_C(5000000017);
    fake.thread_cpu_ns = UINT64_C(6000000019);
    HL_CHECK(hl_production_clock_gettime(&services, HL_PRODUCTION_CLOCK_MONOTONIC, &value) == 0);
    HL_CHECK(value.tv_sec == 3 && value.tv_nsec == 7);
    HL_CHECK(hl_production_clock_gettime(&services, HL_PRODUCTION_CLOCK_REALTIME, &value) == 0);
    HL_CHECK(value.tv_sec == 9 && value.tv_nsec == 11);
    fake.monotonic_ns = UINT64_C(123456789);
    HL_CHECK(hl_production_clock_nanoseconds(&services, HL_PRODUCTION_CLOCK_MONOTONIC, &nanoseconds) == 0);
    HL_CHECK(nanoseconds == UINT64_C(123456789));
    HL_CHECK(services.clock->raw_monotonic_ns(services.context).value == UINT64_C(4000000013));
    {
        hl_host_result frequency = services.clock->architectural_counter_hz(services.context);
        HL_CHECK(frequency.status == HL_STATUS_OK && frequency.value == UINT64_C(24000000));
        fake.architectural_counter_hz = 0;
        frequency = services.clock->architectural_counter_hz(services.context);
        HL_CHECK(frequency.status == HL_STATUS_NOT_SUPPORTED);
        fake.architectural_counter_hz = UINT64_C(24000000);
    }
    HL_CHECK(services.clock->process_cpu_ns(services.context).value == UINT64_C(5000000017));
    HL_CHECK(services.clock->thread_cpu_ns(services.context).value == UINT64_C(6000000019));
    HL_CHECK(services.clock->sleep_until(services.context, HL_HOST_CLOCK_MONOTONIC, UINT64_C(7000000000)).status ==
             HL_STATUS_OK);
    HL_CHECK(fake.monotonic_ns == UINT64_C(7000000000));
    HL_CHECK(services.clock->sleep_until(services.context, HL_HOST_CLOCK_REALTIME, UINT64_C(10000000000)).status ==
             HL_STATUS_OK);
    HL_CHECK(fake.realtime_ns == UINT64_C(10000000000));
    HL_CHECK(services.clock->sleep_until(services.context, HL_HOST_CLOCK_RAW_MONOTONIC, UINT64_C(9000000000)).status ==
             HL_STATUS_OK);
    HL_CHECK(fake.raw_monotonic_ns == UINT64_C(9000000000));
    HL_CHECK(services.clock->sleep_until(services.context, HL_HOST_CLOCK_PROCESS_CPU, 1).status ==
             HL_STATUS_NOT_SUPPORTED);
    HL_CHECK(services.clock->sleep_until(services.context, 99, 1).status == HL_STATUS_NOT_SUPPORTED);
    hl_fake_host_fail_next(&fake, HL_STATUS_INTERRUPTED);
    HL_CHECK(services.clock->sleep_until(services.context, HL_HOST_CLOCK_MONOTONIC, UINT64_C(10000000000)).status ==
             HL_STATUS_INTERRUPTED);
    HL_CHECK(fake.monotonic_ns == UINT64_C(7000000000));
    errno = 0;
    HL_CHECK(hl_production_clock_nanoseconds(&services, 99, &nanoseconds) == -1 && errno == EINVAL);
    hl_fake_host_fail_next(&fake, HL_STATUS_PLATFORM_FAILURE);
    errno = 0;
    HL_CHECK(hl_production_clock_gettime(&services, HL_PRODUCTION_CLOCK_MONOTONIC, &value) == -1 && errno == EIO);
    errno = 0;
    HL_CHECK(hl_production_clock_gettime(NULL, HL_PRODUCTION_CLOCK_MONOTONIC, &value) == -1 && errno == EINVAL);
    return EXIT_SUCCESS;
}
