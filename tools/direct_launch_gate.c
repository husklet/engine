#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted_signal;

static void interrupt_child(int signal_number) {
    interrupted_signal = signal_number;
}

static int install_handlers(void) {
    const int signals[] = {SIGHUP, SIGINT, SIGTERM};
    struct sigaction action = {0};
    action.sa_handler = interrupt_child;
    if (sigemptyset(&action.sa_mask) != 0) return -1;
    for (size_t index = 0; index < sizeof signals / sizeof signals[0]; ++index)
        if (sigaction(signals[index], &action, NULL) != 0) return -1;
    return 0;
}

static int restore_child_handlers(void) {
    const int signals[] = {SIGHUP, SIGINT, SIGTERM};
    struct sigaction action = {0};
    action.sa_handler = SIG_DFL;
    if (sigemptyset(&action.sa_mask) != 0) return -1;
    for (size_t index = 0; index < sizeof signals / sizeof signals[0]; ++index)
        if (sigaction(signals[index], &action, NULL) != 0) return -1;
    return 0;
}

static int copy_guest(const char *source, const char *destination) {
    unsigned char buffer[65536];
    int input = -1;
    int output = -1;
    int result = -1;

    input = open(source, O_RDONLY);
    if (input < 0) goto done;
    output = open(destination, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (output < 0) goto done;
    for (;;) {
        ssize_t count = read(input, buffer, sizeof buffer);
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            goto done;
        }
        size_t offset = 0;
        while (offset < (size_t)count) {
            ssize_t written = write(output, buffer + offset, (size_t)count - offset);
            if (written < 0) {
                if (errno == EINTR) continue;
                goto done;
            }
            if (written == 0) {
                errno = EIO;
                goto done;
            }
            offset += (size_t)written;
        }
    }
    if (fchmod(output, 0700) != 0) goto done;
    result = 0;

done: {
    int saved_errno = errno;
    if (output >= 0 && close(output) != 0 && result == 0) {
        saved_errno = errno;
        result = -1;
    }
    if (input >= 0 && close(input) != 0 && result == 0) {
        saved_errno = errno;
        result = -1;
    }
    errno = saved_errno;
    return result;
}
}

static int run_runner(const char *workspace, char *const arguments[]) {
    pid_t child = fork();
    int status = 0;
    if (child < 0) return -1;
    if (child == 0) {
        if (restore_child_handlers() != 0 || chdir(workspace) != 0) _exit(126);
        execvp(arguments[0], arguments);
        _exit(127);
    }
    pid_t waited;
    do {
        if (interrupted_signal != 0) (void)kill(child, interrupted_signal);
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child) return -1;
    if (interrupted_signal != 0) return 128 + interrupted_signal;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    errno = ECHILD;
    return -1;
}

int main(int argc, char **argv) {
    char *workspace = NULL;
    char *guest_copy = NULL;
    int result = 1;

    if (argc != 6) {
        fprintf(stderr, "usage: %s <cli|config> <runner> <engine> <guest> <expected-exit>\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "cli") != 0 && strcmp(argv[1], "config") != 0) {
        fprintf(stderr, "direct-launch: unsupported mode %s\n", argv[1]);
        return 2;
    }
    if (install_handlers() != 0) {
        fprintf(stderr, "direct-launch: cannot install signal handlers: %s\n", strerror(errno));
        return 1;
    }

    const char *temporary = hl_tool_config_tmpdir();
    size_t workspace_size = strlen(temporary) + sizeof "/hl-direct-launch.XXXXXX";
    workspace = malloc(workspace_size);
    if (workspace == NULL ||
        snprintf(workspace, workspace_size, "%s/hl-direct-launch.XXXXXX", temporary) >= (int)workspace_size ||
        mkdtemp(workspace) == NULL) {
        fprintf(stderr, "direct-launch: cannot create workspace: %s\n", strerror(errno));
        goto done;
    }
    size_t guest_size = strlen(workspace) + sizeof "/guest";
    guest_copy = malloc(guest_size);
    if (guest_copy == NULL || snprintf(guest_copy, guest_size, "%s/guest", workspace) >= (int)guest_size ||
        copy_guest(argv[4], guest_copy) != 0) {
        fprintf(stderr, "direct-launch: cannot stage guest %s: %s\n", argv[4], strerror(errno));
        goto done;
    }

    char *runner_arguments[] = {argv[2], "env", argv[3], guest_copy, argv[5], NULL};
    result = run_runner(workspace, runner_arguments);
    if (result < 0) {
        fprintf(stderr, "direct-launch: cannot run %s: %s\n", argv[2], strerror(errno));
        result = 1;
    } else if (result == 127) {
        fprintf(stderr, "direct-launch: cannot execute %s\n", argv[2]);
    }

done:
    if (guest_copy != NULL && unlink(guest_copy) != 0 && errno != ENOENT && result == 0) result = 1;
    if (workspace != NULL && rmdir(workspace) != 0 && errno != ENOENT && result == 0) result = 1;
    free(guest_copy);
    free(workspace);
    return result;
}
