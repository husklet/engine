#define _GNU_SOURCE
#include "test.h"
#include "hl/linux.h"
#include "hl/linux_abi.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static const hl_host_file_services *native_file;
static _Atomic uint32_t *clone_count, *close_count;

typedef struct churn_thread {
    const hl_host_services *services;
    _Atomic uint32_t stop;
    _Atomic uint32_t operations;
} churn_thread;

typedef struct prepared_child {
    hl_linux_abi *abi;
    hl_linux_fork_plan *plan;
} prepared_child;

static int32_t run_prepared_child(void *opaque) {
    prepared_child *child = opaque;
    char byte;
    if (hl_linux_abi_fork_host_completed(child->plan) != HL_STATUS_OK) return 20;
    if (hl_linux_abi_fork_child(child->abi, child->plan) != HL_STATUS_OK) return 21;
    if (hl_linux_read(child->abi, 9, &byte, 1) != 0) return 22;
    if (hl_linux_close(child->abi, 9) != 0) return 23;
    return 0;
}

static void *churn_mutexes(void *opaque) {
    churn_thread *thread = opaque;
    while (atomic_load(&thread->stop) == 0) {
        hl_host_result created = thread->services->sync->mutex_create(thread->services->context);
        if (created.status == HL_STATUS_OK) {
            (void)thread->services->sync->mutex_close(thread->services->context, created.value);
            atomic_fetch_add(&thread->operations, 1);
        }
    }
    return NULL;
}

static hl_host_result counted_clone(void *context, hl_host_handle file) {
    hl_host_result result = native_file->clone_for_fork(context, file);
    if (result.status == HL_STATUS_OK) atomic_fetch_add(clone_count, 1);
    return result;
}

static hl_host_result counted_close(void *context, hl_host_handle file) {
    hl_host_result result = native_file->close(context, file);
    if (result.status == HL_STATUS_OK) atomic_fetch_add(close_count, 1);
    return result;
}

int main(void) {
    hl_host_linux *host = NULL;
    hl_host_services services;
    hl_host_file_services files;
    hl_linux_abi abi;
    hl_linux_fd_entry fds[16];
    hl_linux_ofd_entry ofds[16];
    hl_linux_fork_record records[16];
    hl_linux_fork_plan plan = {
        .abi = HL_LINUX_ABI_VERSION, .size = sizeof(plan), .records = records, .capacity = HL_ARRAY_COUNT(records)};
    hl_host_result opened;
    hl_host_result spawned;
    hl_host_result waited;
    prepared_child prepared = {&abi, &plan};
    char path[128], bytes[3] = {0};
    churn_thread churn = {0};
    pthread_t locker;
    int ready[2], status;
    pid_t child;
    clone_count = mmap(NULL, 2 * sizeof(*clone_count), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    HL_CHECK(clone_count != MAP_FAILED);
    close_count = clone_count + 1;
    HL_CHECK(hl_host_linux_create(&host, &services) == HL_STATUS_OK);
    native_file = services.file;
    files = *native_file;
    files.clone_for_fork = counted_clone;
    files.close = counted_close;
    services.file = &files;
    snprintf(path, sizeof(path), "/tmp/hl-linux-fork-%ld", (long)getpid());
    opened =
        files.open_relative(services.context, HL_HOST_HANDLE_CWD, path, strlen(path),
                            HL_HOST_FILE_READ | HL_HOST_FILE_WRITE, HL_HOST_FILE_CREATE | HL_HOST_FILE_TRUNCATE, 0600);
    HL_CHECK(opened.status == HL_STATUS_OK);
    HL_CHECK(files.write(services.context, opened.value, "abcdef", 6).status == HL_STATUS_OK);
    HL_CHECK(files.seek(services.context, opened.value, 0, HL_LINUX_SEEK_SET).status == HL_STATUS_OK);
    HL_CHECK(hl_linux_abi_init(&abi, &services, fds, HL_ARRAY_COUNT(fds), ofds, HL_ARRAY_COUNT(ofds)) == HL_STATUS_OK);
    HL_CHECK(hl_linux_fd_install_at(&abi, 9, opened.value, HL_LINUX_O_RDWR, 0) == HL_STATUS_OK);
    /* A failed/cancelled fork uses parent completion: unarm, discard clone, preserve the original OFD. */
    HL_CHECK(hl_linux_abi_fork_prepare(&abi, &plan) == HL_STATUS_OK && plan.armed == 1);
    HL_CHECK(hl_linux_abi_fork_parent(&abi, &plan) == HL_STATUS_OK && plan.armed == 0);
    HL_CHECK(atomic_load(clone_count) == 1 && atomic_load(close_count) == 1);
    churn.services = &services;
    HL_CHECK(pthread_create(&locker, NULL, churn_mutexes, &churn) == 0);
    while (atomic_load(&churn.operations) == 0) {}
    HL_CHECK(hl_linux_abi_fork_prepare(&abi, &plan) == HL_STATUS_OK && plan.count == 1 && plan.armed == 1);
    HL_CHECK(pipe(ready) == 0);
    child = fork();
    HL_CHECK(child >= 0);
    if (child == 0) {
        close(ready[1]);
        if (hl_linux_abi_fork_child(&abi, &plan) != HL_STATUS_OK) _exit(10);
        if (read(ready[0], bytes, 1) != 1) _exit(11);
        if (hl_linux_read(&abi, 9, bytes, 2) != 2 || memcmp(bytes, "cd", 2) != 0) _exit(12);
        if (hl_linux_close(&abi, 9) != 0) _exit(13);
        _exit(0);
    }
    close(ready[0]);
    HL_CHECK(hl_linux_abi_fork_parent(&abi, &plan) == HL_STATUS_OK);
    atomic_store(&churn.stop, 1);
    HL_CHECK(pthread_join(locker, NULL) == 0);
    HL_CHECK(hl_linux_read(&abi, 9, bytes, 2) == 2 && memcmp(bytes, "ab", 2) == 0);
    HL_CHECK(write(ready[1], "x", 1) == 1);
    close(ready[1]);
    HL_CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    HL_CHECK(hl_linux_read(&abi, 9, bytes, 2) == 2 && memcmp(bytes, "ef", 2) == 0);
    /* The explicit process operation consumes the prepared host bracket in both fork branches. */
    HL_CHECK(hl_linux_abi_fork_prepare(&abi, &plan) == HL_STATUS_OK);
    spawned = services.process->spawn_prepared(services.context, run_prepared_child, &prepared);
    HL_CHECK(hl_linux_abi_fork_host_completed(&plan) == HL_STATUS_OK);
    HL_CHECK(hl_linux_abi_fork_parent(&abi, &plan) == HL_STATUS_OK);
    HL_CHECK(spawned.status == HL_STATUS_OK);
    waited = services.process->wait(services.context, spawned.value, HL_HOST_DEADLINE_INFINITE);
    HL_CHECK(waited.status == HL_STATUS_OK && waited.detail == HL_HOST_PROCESS_EXIT_CODE && waited.value == 0);
    HL_CHECK(services.process->close(services.context, spawned.value).status == HL_STATUS_OK);
    HL_CHECK(hl_linux_close(&abi, 9) == 0);
    HL_CHECK(atomic_load(clone_count) == 3 && atomic_load(close_count) == 8);
    HL_CHECK(hl_linux_abi_destroy(&abi) == HL_STATUS_OK);
    unlink(path);
    hl_host_linux_destroy(host);
    munmap(clone_count, 2 * sizeof(*clone_count));
    return 0;
}
