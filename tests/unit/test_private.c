#define _POSIX_C_SOURCE 200809L
#include "test.h"

#include "../../src/host/system.h"

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x1000
#endif

enum {
    TEST_FD = 173,
    OTHER_FD = 174,
    MUTATION_FD = 175,
    SPILL_FD_BASE = 10000,
    SPILL_FD_COUNT = 600,
    STRESS_ROUNDS = 20000
};

static uint64_t process_start(pid_t pid) {
    hl_host_process_info process;
    return hl_host_process_read((int64_t)pid, &process) ? process.start_time_ns : 0;
}

static int write_byte(int descriptor, char value) {
    return write(descriptor, &value, 1) == 1 ? 0 : -1;
}

static int read_byte(int descriptor, char *value) {
    return read(descriptor, value, 1) == 1 ? 0 : -1;
}

static int test_refcounts_and_reuse(void) {
    int64_t pid = (int64_t)getpid();
    uint64_t start = process_start((pid_t)pid);
    if (start == 0 || hl_host_process_fd_private_add(TEST_FD) != 0 || hl_host_process_fd_private_add(TEST_FD) != 0)
        return -1;
    if (!hl_host_process_fd_private_is(pid, start, TEST_FD) || !hl_host_process_fd_private_current(TEST_FD) ||
        hl_host_process_fd_private_is(pid, start, OTHER_FD) || hl_host_process_fd_private_current(OTHER_FD))
        return -1;
    hl_host_process_fd_private_remove(TEST_FD);
    if (!hl_host_process_fd_private_is(pid, start, TEST_FD)) return -1;
    hl_host_process_fd_private_remove(TEST_FD);
    if (hl_host_process_fd_private_is(pid, start, TEST_FD) || hl_host_process_fd_private_current(TEST_FD)) return -1;

    /* Reusing the same numeric descriptor for a guest-visible object must not inherit privacy. */
    int source = open("/dev/null", O_RDONLY);
    if (source < 0 || dup2(source, TEST_FD) != TEST_FD) {
        if (source >= 0) close(source);
        return -1;
    }
    if (source != TEST_FD) close(source);
    if (hl_host_process_fd_private_is(pid, start, TEST_FD)) {
        close(TEST_FD);
        return -1;
    }
    close(TEST_FD);
    return 0;
}

static int test_fork_snapshot(void) {
    int report[2];
    int64_t parent_pid = (int64_t)getpid();
    uint64_t parent_start = process_start((pid_t)parent_pid);
    if (parent_start == 0 || pipe(report) != 0 || hl_host_process_fd_private_add(TEST_FD) != 0 ||
        hl_host_process_fd_private_add(TEST_FD) != 0 || hl_host_process_fd_private_fork_prepare() != 0)
        return -1;
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        close(report[0]);
        if (hl_host_process_fd_private_fork_complete(1) != 0) _exit(2);
        uint64_t child_start = process_start(getpid());
        int ok = child_start != 0 && hl_host_process_fd_private_is((int64_t)getpid(), child_start, TEST_FD) &&
                 hl_host_process_fd_private_is(parent_pid, parent_start, TEST_FD);
        hl_host_process_fd_private_remove(TEST_FD);
        ok = ok && hl_host_process_fd_private_is((int64_t)getpid(), child_start, TEST_FD);
        hl_host_process_fd_private_remove(TEST_FD);
        ok = ok && !hl_host_process_fd_private_is((int64_t)getpid(), child_start, TEST_FD) &&
             hl_host_process_fd_private_is(parent_pid, parent_start, TEST_FD);
        (void)write_byte(report[1], ok ? '1' : '0');
        _exit(ok ? 0 : 1);
    }
    if (hl_host_process_fd_private_fork_complete(0) != 0) return -1;
    close(report[1]);
    char result = 0;
    int status = 0;
    int ok = read_byte(report[0], &result) == 0 && waitpid(child, &status, 0) == child && result == '1' &&
             WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
             hl_host_process_fd_private_is(parent_pid, parent_start, TEST_FD);
    close(report[0]);
    hl_host_process_fd_private_remove(TEST_FD);
    hl_host_process_fd_private_remove(TEST_FD);
    return ok && !hl_host_process_fd_private_is(parent_pid, parent_start, TEST_FD) ? 0 : -1;
}

static int test_process_concurrency(void) {
    int ready[2], done[2];
    if (pipe(ready) != 0 || pipe(done) != 0) return -1;
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        close(ready[0]);
        close(done[1]);
        if (hl_host_process_fd_private_add(TEST_FD) != 0 || write_byte(ready[1], 'R') != 0) _exit(2);
        char command;
        if (read_byte(done[0], &command) != 0) _exit(3);
        for (int round = 0; round < STRESS_ROUNDS; ++round) {
            hl_host_process_fd_private_remove(TEST_FD);
            if (hl_host_process_fd_private_add(TEST_FD) != 0) _exit(4);
        }
        hl_host_process_fd_private_remove(TEST_FD);
        _exit(0);
    }
    close(ready[1]);
    close(done[0]);
    char marker;
    if (read_byte(ready[0], &marker) != 0 || marker != 'R') return -1;
    uint64_t start = process_start(child);
    if (start == 0 || !hl_host_process_fd_private_is((int64_t)child, start, TEST_FD) ||
        hl_host_process_fd_private_is((int64_t)child, start, OTHER_FD) || write_byte(done[1], 'G') != 0)
        return -1;
    for (int round = 0; round < STRESS_ROUNDS; ++round) {
        int visible = hl_host_process_fd_private_is((int64_t)child, start, TEST_FD);
        if ((visible != 0 && visible != 1) || hl_host_process_fd_private_is((int64_t)child, start, OTHER_FD)) return -1;
    }
    int status = 0;
    int ok = waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    close(ready[0]);
    close(done[1]);
    return ok ? 0 : -1;
}

typedef struct worker_fork_case {
    int report;
    int result;
} worker_fork_case;

static void *worker_fork_main(void *opaque) {
    worker_fork_case *test = opaque;
    int64_t parent = (int64_t)getpid();
    uint64_t parent_start = process_start((pid_t)parent);
    if (parent_start == 0 || hl_host_process_fd_private_fork_prepare() != 0) {
        test->result = -1;
        return NULL;
    }
    pid_t child = fork();
    if (child < 0) {
        hl_host_process_fd_private_fork_complete(0);
        test->result = -1;
        return NULL;
    }
    if (child == 0) {
        if (hl_host_process_fd_private_fork_complete(1) != 0) _exit(2);
        uint64_t child_start = process_start(getpid());
        char result = child_start != 0 && hl_host_process_fd_private_is(getpid(), child_start, TEST_FD) &&
                              hl_host_process_fd_private_is(parent, parent_start, TEST_FD)
                          ? '1'
                          : '0';
        (void)write_byte(test->report, result);
        _exit(result == '1' ? 0 : 1);
    }
    if (hl_host_process_fd_private_fork_complete(0) != 0) {
        test->result = -1;
        return NULL;
    }
    int status = 0;
    test->result = waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
    return NULL;
}

static int test_worker_thread_fork(void) {
    int report[2];
    pthread_t worker;
    worker_fork_case test = {.report = -1, .result = -1};
    int64_t pid = (int64_t)getpid();
    uint64_t start = process_start((pid_t)pid);
    if (start == 0 || pipe(report) != 0 || hl_host_process_fd_private_add(TEST_FD) != 0) return -1;
    test.report = report[1];
    if (pthread_create(&worker, NULL, worker_fork_main, &test) != 0) return -1;
    char result = 0;
    int joined = pthread_join(worker, NULL) == 0;
    close(report[1]);
    int ok = joined && read_byte(report[0], &result) == 0 && result == '1' && test.result == 0 &&
             hl_host_process_fd_private_is(pid, start, TEST_FD);
    close(report[0]);
    hl_host_process_fd_private_remove(TEST_FD);
    return ok ? 0 : -1;
}

typedef struct mutation_case {
    _Atomic int begin;
    _Atomic int attempted;
    _Atomic int finished;
    int result;
} mutation_case;

static void *mutation_main(void *opaque) {
    mutation_case *test = opaque;
    while (!atomic_load_explicit(&test->begin, memory_order_acquire))
        sched_yield();
    atomic_store_explicit(&test->attempted, 1, memory_order_release);
    test->result = hl_host_process_fd_private_add(MUTATION_FD);
    atomic_store_explicit(&test->finished, 1, memory_order_release);
    return NULL;
}

static int test_mutation_serialized_across_fork(void) {
    mutation_case test = {0};
    pthread_t mutator;
    int64_t parent = (int64_t)getpid();
    uint64_t parent_start = process_start((pid_t)parent);
    if (parent_start == 0 || hl_host_process_fd_private_add(TEST_FD) != 0 ||
        pthread_create(&mutator, NULL, mutation_main, &test) != 0 || hl_host_process_fd_private_fork_prepare() != 0)
        return -1;
    atomic_store_explicit(&test.begin, 1, memory_order_release);
    while (!atomic_load_explicit(&test.attempted, memory_order_acquire))
        sched_yield();
    for (int spin = 0; spin < 1000; ++spin)
        sched_yield();
    if (atomic_load_explicit(&test.finished, memory_order_acquire)) {
        hl_host_process_fd_private_fork_complete(0);
        (void)pthread_join(mutator, NULL);
        return -1;
    }
    pid_t child = fork();
    if (child < 0) {
        hl_host_process_fd_private_fork_complete(0);
        (void)pthread_join(mutator, NULL);
        return -1;
    }
    if (child == 0) {
        hl_host_process_fd_private_fork_complete(1);
        uint64_t start = process_start(getpid());
        int ok = start != 0 && hl_host_process_fd_private_is(getpid(), start, TEST_FD) &&
                 !hl_host_process_fd_private_is(getpid(), start, MUTATION_FD);
        _exit(ok ? 0 : 1);
    }
    hl_host_process_fd_private_fork_complete(0);
    int status = 0;
    int ok = waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
             pthread_join(mutator, NULL) == 0 && test.result == 0 &&
             hl_host_process_fd_private_is(parent, parent_start, MUTATION_FD);
    hl_host_process_fd_private_remove(MUTATION_FD);
    hl_host_process_fd_private_remove(TEST_FD);
    return ok ? 0 : -1;
}

static int test_cell_spill_and_child_replay(void) {
    int64_t parent = (int64_t)getpid();
    uint64_t parent_start = process_start((pid_t)parent);
    if (parent_start == 0) return -1;
    int added = 0;
    for (; added < SPILL_FD_COUNT; ++added)
        if (hl_host_process_fd_private_add(SPILL_FD_BASE + added) != 0) break;
    if (added != SPILL_FD_COUNT || hl_host_process_fd_private_fork_prepare() != 0) goto failed;
    pid_t child = fork();
    if (child < 0) {
        (void)hl_host_process_fd_private_fork_complete(0);
        goto failed;
    }
    if (child == 0) {
        if (hl_host_process_fd_private_fork_complete(1) != 0) _exit(2);
        uint64_t start = process_start(getpid());
        int ok = start != 0;
        for (int index = 0; ok && index < SPILL_FD_COUNT; ++index)
            ok = hl_host_process_fd_private_is(getpid(), start, SPILL_FD_BASE + index);
        _exit(ok ? 0 : 1);
    }
    if (hl_host_process_fd_private_fork_complete(0) != 0) goto failed;
    int status = 0;
    int ok = waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    for (int index = 0; index < added; ++index)
        hl_host_process_fd_private_remove(SPILL_FD_BASE + index);
    return ok ? 0 : -1;
failed:
    for (int index = 0; index < added; ++index)
        hl_host_process_fd_private_remove(SPILL_FD_BASE + index);
    return -1;
}

static int test_unrelated_process_mutation_does_not_abort_fork(void) {
    struct shared_state {
        _Atomic int ready;
        _Atomic int stop;
    } *shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    if (shared == MAP_FAILED) return -1;
    int added = 0;
    for (; added < SPILL_FD_COUNT; ++added)
        if (hl_host_process_fd_private_add(SPILL_FD_BASE + added) != 0) break;
    if (added != SPILL_FD_COUNT) goto failed;
    pid_t mutator = fork();
    if (mutator < 0) goto failed;
    if (mutator == 0) {
        atomic_store_explicit(&shared->ready, 1, memory_order_release);
        while (!atomic_load_explicit(&shared->stop, memory_order_acquire)) {
            if (hl_host_process_fd_private_add(OTHER_FD) != 0) _exit(2);
            hl_host_process_fd_private_remove(OTHER_FD);
        }
        _exit(0);
    }
    while (!atomic_load_explicit(&shared->ready, memory_order_acquire))
        sched_yield();
    int ok = 1;
    for (int round = 0; round < 2000; ++round) {
        if (hl_host_process_fd_private_fork_prepare() != 0) {
            ok = 0;
            break;
        }
        if (hl_host_process_fd_private_fork_complete(0) != 0) {
            ok = 0;
            break;
        }
    }
    atomic_store_explicit(&shared->stop, 1, memory_order_release);
    int status = 0;
    ok = ok && waitpid(mutator, &status, 0) == mutator && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    for (int index = 0; index < added; ++index)
        hl_host_process_fd_private_remove(SPILL_FD_BASE + index);
    munmap(shared, sizeof(*shared));
    return ok ? 0 : -1;
failed:
    for (int index = 0; index < added; ++index)
        hl_host_process_fd_private_remove(SPILL_FD_BASE + index);
    munmap(shared, sizeof(*shared));
    return -1;
}

int main(void) {
    HL_CHECK(test_refcounts_and_reuse() == 0);
    HL_CHECK(test_fork_snapshot() == 0);
    HL_CHECK(test_process_concurrency() == 0);
    HL_CHECK(test_worker_thread_fork() == 0);
    HL_CHECK(test_mutation_serialized_across_fork() == 0);
    HL_CHECK(test_cell_spill_and_child_replay() == 0);
    HL_CHECK(test_unrelated_process_mutation_does_not_abort_fork() == 0);
    return 0;
}
