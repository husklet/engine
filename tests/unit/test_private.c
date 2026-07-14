#define _POSIX_C_SOURCE 200809L
#include "test.h"

#include "../../src/host/system.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

enum { TEST_FD = 173, OTHER_FD = 174, STRESS_ROUNDS = 20000 };

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
    if (!hl_host_process_fd_private_is(pid, start, TEST_FD) || hl_host_process_fd_private_is(pid, start, OTHER_FD))
        return -1;
    hl_host_process_fd_private_remove(TEST_FD);
    if (!hl_host_process_fd_private_is(pid, start, TEST_FD)) return -1;
    hl_host_process_fd_private_remove(TEST_FD);
    if (hl_host_process_fd_private_is(pid, start, TEST_FD)) return -1;

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
        hl_host_process_fd_private_fork_complete(1);
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
    hl_host_process_fd_private_fork_complete(0);
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

int main(void) {
    HL_CHECK(test_refcounts_and_reuse() == 0);
    HL_CHECK(test_fork_snapshot() == 0);
    HL_CHECK(test_process_concurrency() == 0);
    return 0;
}
