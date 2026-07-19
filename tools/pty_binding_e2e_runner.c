#include "hl/engine.h"
#include "hl/linux_abi.h"
#include "hl/macos.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <util.h>

int main(int argc, char **argv) {
    hl_host_macos *host = NULL;
    hl_host_services services;
    hl_engine_fd_binding binding = {0};
    hl_engine_config config = {0};
    hl_engine_executable executable = {0};
    hl_engine_exit result = {0};
    hl_status run_status = HL_STATUS_OK;
    hl_engine *engine = NULL;
    int master = -1, slave = -1, saved = -1;
    int outcome = 1;
    if (argc != 2) return 64;
    if (openpty(&master, &slave, NULL, NULL, NULL) != 0) return 70;
    saved = dup(STDIN_FILENO);
    if (saved < 0 || dup2(slave, STDIN_FILENO) != STDIN_FILENO) goto done;
    if (hl_host_macos_create(&host, &services) != HL_STATUS_OK) goto done;
    hl_host_result opened = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, argv[1], strlen(argv[1]),
                                                         HL_HOST_FILE_READ | HL_HOST_FILE_NOFOLLOW, 0, 0);
    if (opened.status != HL_STATUS_OK) goto destroy;
    executable = (hl_engine_executable){HL_ENGINE_ABI, sizeof(executable), HL_ENGINE_FD_TRANSFER, 0,
                                        opened.value, NULL, 0};
    hl_host_result adopted = services.file->standard_stream(services.context, STDIN_FILENO);
    if (adopted.status != HL_STATUS_OK) goto destroy;
    binding = (hl_engine_fd_binding){HL_ENGINE_ABI, sizeof(binding), STDIN_FILENO,
                                     HL_LINUX_O_RDWR, 0, HL_ENGINE_FD_TRANSFER, adopted.value};
    config.abi = HL_ENGINE_ABI;
    config.size = sizeof(config);
    config.guest_isa = HL_GUEST_ISA_AARCH64;
    config.fd_bindings = &binding;
    config.fd_binding_count = 1;
    config.executable = &executable;
    if (hl_engine_create(&config, &services, &engine) != HL_STATUS_OK) goto destroy;
    result.abi = HL_ENGINE_ABI;
    result.size = sizeof(result);
    run_status = hl_engine_run(engine, 1, (const char *const *)(argv + 1), &result);
    if (run_status == HL_STATUS_OK &&
        result.kind == HL_ENGINE_EXIT_CODE && result.guest_status == 0)
        outcome = 0;
destroy:
    hl_engine_destroy(engine);
    hl_host_macos_destroy(host);
done:
    if (saved >= 0) { (void)dup2(saved, STDIN_FILENO); close(saved); }
    if (slave >= 0) close(slave);
    if (master >= 0) close(master);
    if (outcome != 0)
        fprintf(stderr, "pty-binding: run=%d kind=%u guest=%d detail=%llu\n", (int)run_status, result.kind,
                result.guest_status, (unsigned long long)result.detail);
    return outcome;
}
