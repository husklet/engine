#include "hl/engine.h"
#include "hl/linux_abi.h"
#include "hl/macos.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef HL_TEST_GUEST_ISA
#error HL_TEST_GUEST_ISA is required
#endif

static hl_host_result open_file(const hl_host_services *services, const char *path, uint32_t access) {
    return services->file->open_relative(services->context, HL_HOST_HANDLE_CWD, path, strlen(path), access,
                                         HL_HOST_FILE_CREATE | HL_HOST_FILE_TRUNCATE, 0600);
}

int main(int argc, char **argv) {
    hl_host_macos *host = NULL;
    hl_host_services services;
    hl_engine_fd_binding bindings[4];
    hl_engine_config config;
    hl_engine_exit result;
    hl_engine *engine = NULL;
    hl_host_result input = {HL_STATUS_NOT_FOUND, 0, HL_HOST_HANDLE_INVALID, 0};
    hl_host_result output = {HL_STATUS_NOT_FOUND, 0, HL_HOST_HANDLE_INVALID, 0};
    hl_host_result floor = {HL_STATUS_NOT_FOUND, 0, HL_HOST_HANDLE_INVALID, 0};
    hl_host_result error = {HL_STATUS_NOT_FOUND, 0, HL_HOST_HANDLE_INVALID, 0};
    hl_host_result checked;
    char input_path[160];
    char output_path[160];
    char floor_path[160];
    char error_path[160];
    char bytes[6];
    int outcome = 1;
    if (argc != 2) return 64;
    if (hl_host_macos_create(&host, &services) != HL_STATUS_OK) return 70;
    snprintf(input_path, sizeof(input_path), "/tmp/hl-stdin-%ld", (long)getpid());
    snprintf(output_path, sizeof(output_path), "/tmp/hl-stdout-%ld", (long)getpid());
    snprintf(floor_path, sizeof(floor_path), "/tmp/hl-floor-%ld", (long)getpid());
    snprintf(error_path, sizeof(error_path), "/tmp/hl-stderr-%ld", (long)getpid());
    input = open_file(&services, input_path, HL_HOST_FILE_READ | HL_HOST_FILE_WRITE);
    output = open_file(&services, output_path, HL_HOST_FILE_READ | HL_HOST_FILE_WRITE);
    floor = open_file(&services, floor_path, HL_HOST_FILE_READ | HL_HOST_FILE_WRITE);
    error = open_file(&services, error_path, HL_HOST_FILE_READ | HL_HOST_FILE_WRITE);
    if (input.status != HL_STATUS_OK || output.status != HL_STATUS_OK || floor.status != HL_STATUS_OK ||
        error.status != HL_STATUS_OK)
        goto close_files;
    checked = services.file->write_at(services.context, input.value, 0, (hl_host_const_bytes){"input", 5});
    if (checked.status != HL_STATUS_OK || checked.value != 5) goto close_files;
    checked = services.file->write_at(services.context, floor.value, 0, (hl_host_const_bytes){"floor", 5});
    if (checked.status != HL_STATUS_OK || checked.value != 5) goto close_files;
    memset(bindings, 0, sizeof(bindings));
    bindings[0] = (hl_engine_fd_binding){HL_ENGINE_ABI,       sizeof(bindings[0]), 0, HL_LINUX_O_RDONLY, 0,
                                         HL_ENGINE_FD_BORROW, input.value};
    bindings[1] = (hl_engine_fd_binding){HL_ENGINE_ABI,       sizeof(bindings[1]), 1, HL_LINUX_O_WRONLY, 0,
                                         HL_ENGINE_FD_BORROW, output.value};
    bindings[2] = (hl_engine_fd_binding){HL_ENGINE_ABI,       sizeof(bindings[2]), 64, HL_LINUX_O_RDONLY, 0,
                                         HL_ENGINE_FD_BORROW, floor.value};
    bindings[3] = (hl_engine_fd_binding){HL_ENGINE_ABI,       sizeof(bindings[3]), 2, HL_LINUX_O_WRONLY, 0,
                                         HL_ENGINE_FD_BORROW, error.value};
    memset(&config, 0, sizeof(config));
    config.abi = HL_ENGINE_ABI;
    config.size = sizeof(config);
    config.guest_isa = HL_TEST_GUEST_ISA;
    config.fd_bindings = bindings;
    config.fd_binding_count = 4;
    {
        hl_status created = hl_engine_create(&config, &services, &engine);
        if (created != HL_STATUS_OK) {
            fprintf(stderr, "stdio: create=%d\n", created);
            goto close_files;
        }
    }
    memset(&result, 0, sizeof(result));
    result.abi = HL_ENGINE_ABI;
    result.size = sizeof(result);
    {
        hl_status ran = hl_engine_run(engine, 1, (const char *const *)(argv + 1), &result);
        if (ran != HL_STATUS_OK || result.kind != HL_ENGINE_EXIT_CODE || result.guest_status != 0) {
            fprintf(stderr, "stdio: run=%d kind=%u guest=%d\n", ran, result.kind, result.guest_status);
            goto destroy;
        }
    }
    checked = services.file->read_at(services.context, output.value, 0, (hl_host_bytes){bytes, sizeof(bytes)});
    if (checked.status != HL_STATUS_OK || checked.value != sizeof(bytes) || memcmp(bytes, "output", sizeof(bytes))) {
        fprintf(stderr, "stdio: verify=%d size=%llu bytes=%.*s\n", checked.status, (unsigned long long)checked.value,
                (int)sizeof(bytes), bytes);
        goto destroy;
    }
    checked = services.file->read_at(services.context, input.value, 0, (hl_host_bytes){bytes, 5});
    if (checked.status != HL_STATUS_OK || checked.value != 5 || memcmp(bytes, "input", 5)) goto destroy;
    checked = services.file->read_at(services.context, error.value, 0, (hl_host_bytes){bytes, 5});
    if (checked.status != HL_STATUS_OK || checked.value != 5 || memcmp(bytes, "error", 5)) goto destroy;
    outcome = 0;
destroy:
    hl_engine_destroy(engine);
close_files:
    if (input.status == HL_STATUS_OK) (void)services.file->close(services.context, input.value);
    if (output.status == HL_STATUS_OK) (void)services.file->close(services.context, output.value);
    if (floor.status == HL_STATUS_OK) (void)services.file->close(services.context, floor.value);
    if (error.status == HL_STATUS_OK) (void)services.file->close(services.context, error.value);
    unlink(input_path);
    unlink(output_path);
    unlink(floor_path);
    unlink(error_path);
    hl_host_macos_destroy(host);
    return outcome;
}
