#include "../../core/engine_backend.h"

#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef HL_PRODUCTION_GUEST_ISA
#error HL_PRODUCTION_GUEST_ISA is required
#endif

int hl_run_linux_guest(const char *rootfs, int argc, char *const argv[]);

static hl_status hl_production_run_process(const char *rootfs, int argc, const char *const argv[],
                                           hl_engine_exit *result) {
    pid_t pid = fork();
    int status;
    if (pid < 0) return HL_STATUS_PLATFORM_FAILURE;
    if (pid == 0) {
        int code = hl_run_linux_guest(rootfs, argc, (char *const *)(uintptr_t)argv);
        _exit(code & 255);
    }
    do {
        if (waitpid(pid, &status, 0) == pid) break;
        if (errno != EINTR) return HL_STATUS_PLATFORM_FAILURE;
    } while (1);
    result->detail = 0;
    if (WIFEXITED(status)) {
        result->kind = HL_ENGINE_EXIT_CODE;
        result->guest_status = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result->kind = HL_ENGINE_EXIT_SIGNAL;
        result->guest_status = WTERMSIG(status);
    } else {
        result->kind = HL_ENGINE_EXIT_ENGINE_ERROR;
        result->guest_status = status;
    }
    return HL_STATUS_OK;
}

static const hl_engine_backend backend = {HL_PRODUCTION_GUEST_ISA, hl_production_run_process};

__attribute__((constructor)) static void hl_production_register_backend(void) {
    hl_engine_backend_register(&backend);
}
