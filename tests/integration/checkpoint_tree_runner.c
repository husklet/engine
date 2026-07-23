#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { TIMEOUT_SECONDS = 15 };

static int exists(const char *path) { return access(path, F_OK) == 0; }

static int output_has(const char *path, const char *needle) {
    char data[16384];
    int fd = open(path, O_RDONLY);
    ssize_t size;
    if (fd < 0) return 0;
    size = read(fd, data, sizeof(data) - 1);
    close(fd);
    if (size < 0) return 0;
    data[size] = 0;
    return strstr(data, needle) != NULL;
}

static int wait_for_path(const char *path, time_t deadline) {
    struct timespec tick = {0, 10000000};
    while (time(NULL) < deadline) {
        if (exists(path)) return 0;
        nanosleep(&tick, NULL);
    }
    return -1;
}

static int wait_for_ready(const char *path, int processes, time_t deadline) {
    struct timespec tick = {0, 10000000};
    while (time(NULL) < deadline) {
        if (output_has(path, "READY 1") && (processes == 1 || output_has(path, "READY 2")) &&
            (processes < 3 || output_has(path, "READY 3"))) return 0;
        nanosleep(&tick, NULL);
    }
    return -1;
}

static int wait_for_restored(const char *path, int pipe_case, int deleted_case, int threads_case, int memfd_case,
                             int eventfd_case, int timerfd_case, int inotify_case, time_t deadline) {
    struct timespec tick = {0, 10000000};
    while (time(NULL) < deadline) {
        if (inotify_case == 5) {
            if (output_has(path, "CONNECTED-SOCKET-RESTORED")) return 0;
        } else if (inotify_case == 6) {
            if (output_has(path, "SIGNAL-RESTORED")) return 0;
        } else if (inotify_case == 4) {
            if (output_has(path, "SOCKET-STATE-RESTORED")) return 0;
        } else if (inotify_case == 3) {
            if (output_has(path, "SOCKETPAIR-RESTORED")) return 0;
        } else if (inotify_case == 2) {
            if (output_has(path, "EPOLL-RESTORED")) return 0;
        } else if (inotify_case) {
            if (output_has(path, "INOTIFY-RESTORED")) return 0;
        } else if (timerfd_case) {
            if (output_has(path, "TIMERFD-RESTORED")) return 0;
        } else if (eventfd_case) {
            if (output_has(path, "EVENTFD-RESTORED")) return 0;
        } else if (memfd_case) {
            if (output_has(path, "MEMFD-RESTORED")) return 0;
        } else if (threads_case) {
            if (output_has(path, "THREADS-RESTORED")) return 0;
        } else if (deleted_case) {
            if (output_has(path, "DELETED-RESTORED")) return 0;
        } else if (pipe_case) {
            if (output_has(path, "PIPE-RESTORED")) return 0;
        } else if (output_has(path, "RESTORED 1 ") && output_has(path, "RESTORED 2 ") &&
                   output_has(path, "RESTORED 3 ") && output_has(path, "TREE-RESTORED ")) {
            return 0;
        }
        nanosleep(&tick, NULL);
    }
    return -1;
}

static int wait_child(pid_t pid, time_t deadline) {
    struct timespec tick = {0, 10000000};
    int status;
    while (time(NULL) < deadline) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
        if (result < 0 && errno != EINTR) return 125;
        nanosleep(&tick, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return 124;
}

static pid_t launch(const char *engine, const char *guest, const char *release,
                    const char *output, const char *checkpoint, int restore, int permissive) {
    pid_t pid = fork();
    if (pid != 0) return pid;
    if (!restore && setsid() < 0) _exit(126);
    {
        int fd = open(output, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) _exit(126);
        if (fd != STDOUT_FILENO) close(fd);
    }
    if (restore && permissive)
        execl(engine, engine, "--restore-policy", "discard-optional", "--restore", checkpoint, (char *)NULL);
    else if (restore)
        execl(engine, engine, "--restore", checkpoint, (char *)NULL);
    else
        execl(engine, engine, "--checkpoint", checkpoint, guest, release, (char *)NULL);
    _exit(127);
}

int main(int argc, char **argv) {
    char temporary[1024];
    char checkpoint[512], trigger[520], manifest[528], output[512], release[512], release_error[520];
    uint32_t generation = 1;
    pid_t child;
    int fd, result;
    int pipe_case = argc == 4 && strcmp(argv[3], "pipe") == 0;
    int deleted_case = argc == 4 && strcmp(argv[3], "deleted") == 0;
    int threads_case = argc == 4 && strcmp(argv[3], "threads") == 0;
    int memfd_case = argc == 4 && strcmp(argv[3], "memfd") == 0;
    int eventfd_case = argc == 4 && strcmp(argv[3], "eventfd") == 0;
    int timerfd_case = argc == 4 && strcmp(argv[3], "timerfd") == 0;
    int inotify_case = argc == 4 && strcmp(argv[3], "inotify") == 0;
    int epoll_case = argc == 4 && strcmp(argv[3], "epoll-checkpoint") == 0;
    int socketpair_case = argc == 4 && strcmp(argv[3], "socketpair") == 0;
    int socket_state_case = argc == 4 && strcmp(argv[3], "socket-state") == 0;
    int connected_socket_case = argc == 4 && strcmp(argv[3], "connected-socket") == 0;
    int signal_case = argc == 4 && strcmp(argv[3], "signal-state") == 0;
    int connecting_refusal_case = argc == 4 && strcmp(argv[3], "connecting-refusal") == 0;
    int corrupt_magic_case = argc == 4 && strcmp(argv[3], "corrupt-magic") == 0;
    int corrupt_truncated_case = argc == 4 && strcmp(argv[3], "corrupt-truncated") == 0;
    int permissive_case = argc == 4 && strcmp(argv[3], "missing-external") == 0;
    int modified_external_case = argc == 4 && strcmp(argv[3], "modified-external") == 0;
    if ((argc != 3 && !pipe_case && !deleted_case && !threads_case && !memfd_case && !eventfd_case &&
         !timerfd_case && !inotify_case && !epoll_case && !socketpair_case && !socket_state_case &&
         !connected_socket_case && !signal_case && !connecting_refusal_case && !corrupt_magic_case &&
         !corrupt_truncated_case && !permissive_case && !modified_external_case) ||
        getcwd(temporary, sizeof temporary) == NULL ||
        strlen(temporary) + sizeof("/build/hl-checkpoint-tree.XXXXXX") > sizeof temporary)
        return 2;
    strcat(temporary, "/build/hl-checkpoint-tree.XXXXXX");
    if (mkdtemp(temporary) == NULL) return 2;
    snprintf(checkpoint, sizeof checkpoint, "%s/image", temporary);
    snprintf(trigger, sizeof trigger, "%s.trigger", checkpoint);
    snprintf(manifest, sizeof manifest, "%s/MANIFEST", checkpoint);
    snprintf(output, sizeof output, "%s/release.output", temporary);
    snprintf(release, sizeof release, "%s/release", temporary);
    snprintf(release_error, sizeof release_error, "%s.error", release);

    child = launch(argv[1], argv[2], release, output, checkpoint, 0, permissive_case);
    if (child < 0) return 3;
    if (wait_for_ready(output, (deleted_case || threads_case || memfd_case || inotify_case || epoll_case ||
                                signal_case || connecting_refusal_case || modified_external_case) ? 1 :
                                   (socketpair_case || connected_socket_case) ? 2 : socket_state_case ? 1 :
                                   (pipe_case || eventfd_case || timerfd_case || permissive_case) ? 2 : 3,
                       time(NULL) + TIMEOUT_SECONDS) != 0) {
        fprintf(stderr, "checkpoint runner: readiness timeout one=%d two=%d three=%d\n",
                output_has(output, "READY 1"), output_has(output, "READY 2"), output_has(output, "READY 3"));
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        return 3;
    }
    fd = open(trigger, O_WRONLY);
    if (fd < 0 || pwrite(fd, &generation, sizeof generation, 0) != sizeof generation || fsync(fd) != 0)
        return 4;
    close(fd);
#ifdef SIGINFO
    if (kill(child, SIGINFO) != 0) return 5;
#else
    if (kill(child, SIGURG) != 0) return 5;
#endif
    result = wait_child(child, time(NULL) + TIMEOUT_SECONDS);
    if (connecting_refusal_case) {
        if (result != 70 || exists(manifest) || !output_has(release_error, "connected/in-progress socket")) {
            fprintf(stderr, "checkpoint runner: connecting refusal result=%d manifest=%d diagnostic=%d\n",
                    result, exists(manifest), output_has(release_error, "connected/in-progress socket"));
            return 6;
        }
        printf("checkpoint connecting-socket refusal: ok\n");
        return 0;
    }
    if (result != 0 || wait_for_path(manifest, time(NULL) + TIMEOUT_SECONDS) != 0) return 6;

    if (permissive_case) {
        char external[640];
        snprintf(external, sizeof external, "%s.external", release);
        if (unlink(external) != 0) return 7;
    }
    if (modified_external_case) {
        char external[640];
        snprintf(external, sizeof external, "%s.external", release);
        fd = open(external, O_WRONLY | O_TRUNC);
        if (fd < 0 || write(fd, "after", 5) != 5 || fsync(fd) != 0) return 7;
        close(fd);
    }

    fd = open(release, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || fsync(fd) != 0) return 7;
    close(fd);
    fd = open(temporary, O_RDONLY);
    if (fd < 0 || fsync(fd) != 0) return 7;
    close(fd);
    if (corrupt_magic_case) {
        unsigned char corrupt = 0;
        fd = open(manifest, O_RDWR);
        if (fd < 0 || pread(fd, &corrupt, 1, 0) != 1 || (corrupt ^= 0xffu, pwrite(fd, &corrupt, 1, 0)) != 1 ||
            fsync(fd) != 0)
            return 7;
        close(fd);
    }
    if (corrupt_truncated_case) {
        char records[640];
        struct stat status;
        snprintf(records, sizeof records, "%s/proc.1/fds", checkpoint);
        if (stat(records, &status) != 0 || status.st_size < 1 || truncate(records, status.st_size - 1) != 0)
            return 7;
    }
    child = launch(argv[1], argv[2], release, output, checkpoint, 1, permissive_case);
    if (child < 0) return 8;
    result = wait_child(child, time(NULL) + TIMEOUT_SECONDS);
    if (corrupt_magic_case || corrupt_truncated_case) {
        if (result == 0 || result == 124 || output_has(output, "TREE-RESTORED")) {
            fprintf(stderr, "checkpoint runner: corrupt image accepted mode=%s result=%d restored=%d\n",
                    corrupt_magic_case ? "magic" : "truncated", result, output_has(output, "TREE-RESTORED"));
            return 8;
        }
        printf("checkpoint corrupt-%s rejection: ok\n", corrupt_magic_case ? "magic" : "truncated");
        return 0;
    }
    if (result != 0) {
        fprintf(stderr, "checkpoint runner: restore exited %d\n", result);
        return 8;
    }
    if (permissive_case) {
        char report[640];
        snprintf(report, sizeof report, "%s/RECOVERY.jsonl", checkpoint);
        if (!output_has(output, "PARENT-RESTORED") || output_has(output, "CHILD-RESTORED") ||
            !output_has(report, "\"outcome\":\"stopped\"") || !output_has(report, "required external path"))
            return 9;
        printf("checkpoint missing-external pruning: ok\n");
        return 0;
    }
    if (modified_external_case) {
        char report[640];
        snprintf(report, sizeof report, "%s/RECOVERY.jsonl", checkpoint);
        if (!output_has(output, "MODIFIED-EXTERNAL-RESTORED") ||
            !output_has(report, "\"outcome\":\"reconnected\""))
            return 9;
        printf("checkpoint modified-external current-state restore: ok\n");
        return 0;
    }
    if (wait_for_restored(output, pipe_case, deleted_case, threads_case, memfd_case, eventfd_case, timerfd_case,
                          signal_case ? 6 : connected_socket_case ? 5 : socket_state_case ? 4 : socketpair_case ? 3 :
                          epoll_case ? 2 : inotify_case,
                          time(NULL) + TIMEOUT_SECONDS) != 0)
        return 9;
    printf("checkpoint %s restore: ok\n",
           signal_case ? "signal" : connected_socket_case ? "connected-socket" : socket_state_case ? "socket-state" :
           socketpair_case ? "socketpair" : epoll_case ? "epoll" : inotify_case ? "inotify" : timerfd_case ? "timerfd" : eventfd_case ? "eventfd" : memfd_case ? "memfd" : threads_case ? "threads" : deleted_case ? "deleted" : pipe_case ? "pipe" : "tree");
    return 0;
}
