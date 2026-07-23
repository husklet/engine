#define _POSIX_C_SOURCE 200809L
#include "test.h"

#include "hl/macos.h"
#include "../../src/host/system.h"

#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

static atomic_uint callbacks;

static void notified(void *context, uint64_t token) {
    (void)context;
    if (token == 91) atomic_fetch_add_explicit(&callbacks, 1, memory_order_relaxed);
}

static size_t descriptor_count(void) {
    hl_host_process_fd entries[1024];
    size_t count = 0;
    HL_CHECK(hl_host_process_fds(getpid(), entries, HL_ARRAY_COUNT(entries), &count));
    HL_CHECK(count <= HL_ARRAY_COUNT(entries));
    return count;
}

int main(void) {
    enum { REPEATS = 20 };

    size_t baseline = descriptor_count();
    for (uint32_t iteration = 0; iteration < REPEATS; ++iteration) {
        hl_host_macos *host = NULL;
        hl_host_services services;
        hl_host_result counter;
        hl_host_result subscription;
        hl_host_result channels;
        hl_host_result directory;
        hl_host_result duplicate;
        struct timespec settle = {0, 1000000};
        unsigned before;

        HL_CHECK(hl_host_macos_create(&host, &services) == HL_STATUS_OK);
        counter = services.counter->create(services.context, 0, 0);
        HL_CHECK(counter.status == HL_STATUS_OK);
        subscription = services.counter->subscribe(services.context, counter.value, notified, NULL, 91);
        HL_CHECK(subscription.status == HL_STATUS_OK);
        channels = services.transfer->channel_pair(services.context);
        HL_CHECK(channels.status == HL_STATUS_OK);
        directory = services.directory->create(services.context);
        HL_CHECK(directory.status == HL_STATUS_OK);
        duplicate = services.directory->duplicate(services.context, directory.value);
        HL_CHECK(duplicate.status == HL_STATUS_OK);
        HL_CHECK(services.counter->write(services.context, counter.value, 1).status == HL_STATUS_OK);
        for (uint32_t spin = 0; spin < 1000 && atomic_load_explicit(&callbacks, memory_order_relaxed) == iteration;
             ++spin)
            nanosleep(&settle, NULL);
        HL_CHECK(atomic_load_explicit(&callbacks, memory_order_relaxed) == iteration + 1u);
        hl_host_macos_destroy(host);
        before = atomic_load_explicit(&callbacks, memory_order_relaxed);
        nanosleep(&settle, NULL);
        HL_CHECK(atomic_load_explicit(&callbacks, memory_order_relaxed) == before);
        HL_CHECK(descriptor_count() == baseline);
    }
    return EXIT_SUCCESS;
}
