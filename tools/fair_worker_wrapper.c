/* fair_worker_wrapper.c -- apples-to-apples process-model harness.
 *
 * PURPOSE (fairness). hl-engine runs a guest in a two-process model: a parent
 * dispatcher forks a WORKER child which runs the guest, then waitpid()s it and
 * relays the raw wait status back over a pipe (see src/linux_abi/fork.c). Running
 * a native aarch64 binary directly, or under qemu-aarch64, is a SINGLE process:
 * no fork, no worker, no wait, no status pipe. So a raw native-vs-hl ratio
 * charges hl for process-structure overhead that native/qemu never pay.
 *
 * This wrapper imposes hl's parent/worker/wait/status-pipe structure on ANY
 * command so the cost cancels out. It faithfully mirrors fork.c's shape:
 *
 *   parent (client)  --forks-->  supervisor  --forks-->  runner
 *   runner:      execvp(payload)                         (the guest)
 *   supervisor:  write(pipe, runner_pid i32);
 *                waitpid(runner, &status);
 *                write(pipe, status i32);  _exit
 *   parent:      read(pipe, runner_pid); read(pipe, status);
 *                waitpid(supervisor); decode status; _exit(guest exit code)
 *
 * That is the same double-fork + pid/status wire (fork.c: "server -> client:
 * [i32 runner_pid] then [i32 wait_status]") the engine pays per launch. The
 * wrapper's own translation cost is zero, so wrapped-native minus direct-native
 * is exactly the process-model tax, and all three targets pay it identically.
 *
 * Usage:  fair-worker-wrapper -- COMMAND [ARG ...]
 *         fair-worker-wrapper COMMAND [ARG ...]
 * Exit code and stdout/stderr are those of the guest COMMAND (byte-identical
 * decode path), so perf-runner's --expect check stays valid across targets.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static ssize_t write_all(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        done += (size_t)w;
    }
    return (ssize_t)done;
}

static ssize_t read_all(int fd, void *buf, size_t n) {
    char *p = (char *)buf;
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, p + done, n - done);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break; /* EOF */
        done += (size_t)r;
    }
    return (ssize_t)done;
}

static pid_t reap(pid_t pid, int *status) {
    pid_t w;
    do {
        w = waitpid(pid, status, 0);
    } while (w < 0 && errno == EINTR);
    return w;
}

int main(int argc, char **argv) {
    char **command;
    int pfd[2];
    pid_t supervisor;
    int32_t runner_pid = -1;
    int32_t wire_status = 0;
    int sup_status = 0;

    /* Locate the payload command (accept both "-- CMD" and bare "CMD"). */
    {
        int i = 1;
        if (i < argc && strcmp(argv[i], "--") == 0) i++;
        if (i >= argc) {
            static const char msg[] =
                "usage: fair-worker-wrapper [--] COMMAND [ARG ...]\n";
            (void)write_all(STDERR_FILENO, msg, sizeof(msg) - 1);
            return 2;
        }
        command = &argv[i];
    }

    if (pipe(pfd) != 0) return 70;

    supervisor = fork();
    if (supervisor < 0) return 70;

    if (supervisor == 0) {
        /* SUPERVISOR: owns the control pipe write end; forks the runner. */
        pid_t runner;
        int32_t pid_wire;
        int rstatus = 0;

        close(pfd[0]);
        runner = fork();
        if (runner < 0) _exit(70);
        if (runner == 0) {
            /* RUNNER: becomes the guest, exactly like fork.c's runner execs
             * the loaded image and takes the guest's own _exit path. */
            close(pfd[1]);
            execvp(command[0], command);
            _exit(127);
        }
        /* Report runner pid, then reap it and report its raw wait status --
         * the two i32 messages fork.c sends over its control connection. */
        pid_wire = (int32_t)runner;
        (void)write_all(pfd[1], &pid_wire, sizeof(pid_wire));
        (void)reap(runner, &rstatus);
        {
            int32_t s = (int32_t)rstatus;
            (void)write_all(pfd[1], &s, sizeof(s));
        }
        close(pfd[1]);
        _exit(0);
    }

    /* PARENT (client): read runner pid, then raw status; reap supervisor. */
    close(pfd[1]);
    (void)read_all(pfd[0], &runner_pid, sizeof(runner_pid));
    (void)read_all(pfd[0], &wire_status, sizeof(wire_status));
    close(pfd[0]);
    (void)reap(supervisor, &sup_status);

    /* Decode the guest's status just like the client does, and re-raise it so
     * the caller (perf-runner) sees the guest's real exit code / signal. */
    if (WIFEXITED(wire_status)) return WEXITSTATUS(wire_status);
    if (WIFSIGNALED(wire_status)) return 128 + WTERMSIG(wire_status);
    return 1;
}
