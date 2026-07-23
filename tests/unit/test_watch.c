#include "test.h"

#include "../../src/linux_abi/watch.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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

typedef struct churner {
    hl_linux_watch_set *set;
    uint64_t token;
    atomic_int stop;
} churner;

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

static void *churn(void *opaque) {
    churner *job = opaque;
    uint64_t size = 1;
    while (!atomic_load_explicit(&job->stop, memory_order_relaxed))
        (void)hl_linux_watch_enqueue(job->set, job->token, size++, 1);
    return NULL;
}

int main(void) {
    hl_linux_watch_set set = {0};
    uint64_t first, alias, reused, stale;
    int created, removed;
    observed value = {0};
    pthread_t threads[4];
    producer jobs[4];
    hl_linux_watch_fork_record fork_records[4];
    hl_linux_watch_fork_plan fork_plan = {fork_records, 4, 0};
    churner fork_churn;
    pthread_t fork_thread;
    pid_t child;
    int child_status = 0;
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
    HL_CHECK(hl_linux_watch_fork_prepare(&set, &fork_plan) == HL_STATUS_OK);
    HL_CHECK(hl_linux_watch_fork_child(&set, &fork_plan, rebuild, &value) == HL_STATUS_OK);
    HL_CHECK(value.count == 1 && value.token != reused && value.old_size == 8 && value.new_size == 10);
    reused = value.token;
    HL_CHECK(hl_linux_watch_enqueue(&set, reused, 21, 1) == HL_STATUS_OK);

    for (index = 0; index < 4; ++index) {
        jobs[index] = (producer){&set, reused, 1000u + index * 1000u};
        HL_CHECK(pthread_create(&threads[index], NULL, produce, &jobs[index]) == 0);
    }
    for (index = 0; index < 4; ++index)
        HL_CHECK(pthread_join(threads[index], NULL) == 0);
    value = (observed){0};
    HL_CHECK(hl_linux_watch_drain(&set, observe, &value, &count) == HL_STATUS_OK && count == 1);
    HL_CHECK(value.count == 1 && value.old_size == 20 && value.flags == 15);

    /* A real multithreaded fork: prepare snapshots and locks in the parent;
       the child allocates nothing, resets the inherited mutex, rebuilds, and
       proves the queue can be entered again. */
    fork_churn = (churner){.set = &set, .token = reused, .stop = ATOMIC_VAR_INIT(0)};
    HL_CHECK(pthread_create(&fork_thread, NULL, churn, &fork_churn) == 0);
    fork_plan.count = 0;
    HL_CHECK(hl_linux_watch_fork_prepare(&set, &fork_plan) == HL_STATUS_OK);
    child = fork();
    if (child == 0) {
        observed child_value = {0};
        size_t child_count = 0;
        if (hl_linux_watch_fork_child(&set, &fork_plan, rebuild, &child_value) != HL_STATUS_OK ||
            child_value.count != 1 || hl_linux_watch_enqueue(&set, child_value.token, 77, 4) != HL_STATUS_OK ||
            hl_linux_watch_drain(&set, observe, &child_value, &child_count) != HL_STATUS_OK || child_count != 1)
            _exit(91);
        _exit(0);
    }
    HL_CHECK(child > 0);
    hl_linux_watch_fork_parent(&set);
    atomic_store_explicit(&fork_churn.stop, 1, memory_order_relaxed);
    HL_CHECK(pthread_join(fork_thread, NULL) == 0);
    for (index = 0; index < 200; ++index) {
        pid_t waited = waitpid(child, &child_status, WNOHANG);
        if (waited == child) break;
        HL_CHECK(waited == 0);
        struct timespec pause = {0, 1000000};
        (void)nanosleep(&pause, NULL);
    }
    if (index == 200) {
        (void)kill(child, SIGKILL);
        (void)waitpid(child, &child_status, 0);
        HL_CHECK(0);
    }
    HL_CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);

    hl_linux_watch_shutdown(&set);
    HL_CHECK(hl_linux_watch_enqueue(&set, reused, 30, 1) == HL_STATUS_INTERRUPTED);
    HL_CHECK(hl_linux_watch_retain(&set, 1, 2, 3, &first, &created) == HL_STATUS_INTERRUPTED);
    hl_linux_watch_close(&set);
    return 0;
}
