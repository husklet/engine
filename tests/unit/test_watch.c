#include "test.h"

#include "../../src/linux_abi/watch.h"

#include <pthread.h>
#include <stdint.h>

typedef struct observed {
    uint64_t token;
    uint64_t old_size;
    uint64_t new_size;
    uint32_t flags;
    size_t count;
} observed;

typedef struct producer {
    hl_linux_watch_set *set;
    uint64_t token;
    uint64_t base;
} producer;

static void observe(void *opaque, const hl_linux_watch_change *change) {
    observed *value = opaque;
    value->token = change->token;
    value->old_size = change->old_size;
    value->new_size = change->new_size;
    value->flags = change->flags;
    value->count++;
}

static void rebuild(void *opaque, uint64_t token, uint64_t device, uint64_t object) {
    observed *value = opaque;
    value->token = token;
    value->old_size = device;
    value->new_size = object;
    value->count++;
}

static void *produce(void *opaque) {
    producer *job = opaque;
    uint64_t index;
    for (index = 0; index < 1000; ++index)
        (void)hl_linux_watch_enqueue(job->set, job->token, job->base + index, (uint32_t)(1u << (index & 3u)));
    return NULL;
}

int main(void) {
    hl_linux_watch_set set = {0};
    uint64_t first, alias, reused, stale;
    int created, removed;
    observed value = {0};
    pthread_t threads[4];
    producer jobs[4];
    size_t index;
    size_t count;

    HL_CHECK(hl_linux_watch_init(&set) == HL_STATUS_OK);
    HL_CHECK(hl_linux_watch_retain(&set, 7, 9, 100, &first, &created) == HL_STATUS_OK && created);
    HL_CHECK(hl_linux_watch_retain(&set, 7, 9, 999, &alias, &created) == HL_STATUS_OK && !created);
    HL_CHECK(alias == first);
    HL_CHECK(hl_linux_watch_enqueue(&set, first, 80, 1) == HL_STATUS_OK);
    HL_CHECK(hl_linux_watch_enqueue(&set, first, 60, 2) == HL_STATUS_OK);
    HL_CHECK(hl_linux_watch_drain(&set, observe, &value, &count) == HL_STATUS_OK && count == 1);
    HL_CHECK(value.count == 1 && value.old_size == 100 && value.new_size == 60 && value.flags == 3);

    HL_CHECK(hl_linux_watch_enqueue(&set, first, 44, 8) == HL_STATUS_OK);
    HL_CHECK(hl_linux_watch_release(&set, first, &removed) == HL_STATUS_OK && !removed);
    HL_CHECK(hl_linux_watch_release(&set, alias, &removed) == HL_STATUS_OK && removed);
    stale = first;
    HL_CHECK(hl_linux_watch_enqueue(&set, stale, 44, 8) == HL_STATUS_NOT_FOUND);
    HL_CHECK(hl_linux_watch_retain(&set, 8, 10, 20, &reused, &created) == HL_STATUS_OK && created);
    HL_CHECK(reused != stale);
    HL_CHECK(hl_linux_watch_enqueue(&set, stale, 1, 1) == HL_STATUS_NOT_FOUND);
    value = (observed){0};
    HL_CHECK(hl_linux_watch_drain(&set, observe, &value, &count) == HL_STATUS_OK && count == 0);

    value = (observed){0};
    hl_linux_watch_fork_prepare(&set);
    HL_CHECK(hl_linux_watch_fork_child(&set, rebuild, &value) == HL_STATUS_OK);
    HL_CHECK(value.count == 1 && value.token != reused && value.old_size == 8 && value.new_size == 10);
    reused = value.token;
    HL_CHECK(hl_linux_watch_enqueue(&set, reused, 21, 1) == HL_STATUS_OK);

    for (index = 0; index < 4; ++index) {
        jobs[index] = (producer){&set, reused, 1000u + index * 1000u};
        HL_CHECK(pthread_create(&threads[index], NULL, produce, &jobs[index]) == 0);
    }
    for (index = 0; index < 4; ++index) HL_CHECK(pthread_join(threads[index], NULL) == 0);
    value = (observed){0};
    HL_CHECK(hl_linux_watch_drain(&set, observe, &value, &count) == HL_STATUS_OK && count == 1);
    HL_CHECK(value.count == 1 && value.old_size == 20 && value.flags == 15);

    hl_linux_watch_shutdown(&set);
    HL_CHECK(hl_linux_watch_enqueue(&set, reused, 30, 1) == HL_STATUS_INTERRUPTED);
    HL_CHECK(hl_linux_watch_retain(&set, 1, 2, 3, &first, &created) == HL_STATUS_INTERRUPTED);
    hl_linux_watch_close(&set);
    return 0;
}
