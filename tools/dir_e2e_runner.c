#include "hl/engine.h"
#include "hl/linux_abi.h"
#include "hl/macos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef HL_TEST_GUEST_ISA
#error HL_TEST_GUEST_ISA is required
#endif

int main(int argc, char **argv) {
    hl_host_macos *host = NULL;
    hl_host_services services;
    hl_engine_fd_binding binding;
    hl_engine_config config;
    hl_engine_exit result;
    hl_engine *engine = NULL;
    hl_host_result directory = {HL_STATUS_NOT_FOUND, 0, HL_HOST_HANDLE_INVALID, 0};
    hl_host_result input = {HL_STATUS_NOT_FOUND, 0, HL_HOST_HANDLE_INVALID, 0};
    hl_host_result output = {HL_STATUS_NOT_FOUND, 0, HL_HOST_HANDLE_INVALID, 0};
    hl_host_result checked;
    char directory_path[] = "/tmp/hl-dir-XXXXXX";
    char bytes[5];
    int outcome = 1;
    if (argc != 2) return 64;
    if (hl_host_macos_create(&host, &services) != HL_STATUS_OK) return 70;
    if (mkdtemp(directory_path) == NULL) goto done;
    directory = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, directory_path,
                                             strlen(directory_path), HL_HOST_FILE_READ | HL_HOST_FILE_DIRECTORY, 0, 0);
    if (directory.status != HL_STATUS_OK) goto remove_directory;
    input = services.file->open_relative(services.context, directory.value, "input", 5,
                                         HL_HOST_FILE_READ | HL_HOST_FILE_WRITE,
                                         HL_HOST_FILE_CREATE | HL_HOST_FILE_TRUNCATE, 0600);
    if (input.status != HL_STATUS_OK) goto close_handles;
    checked = services.file->write_at(services.context, input.value, 0, (hl_host_const_bytes){"hello", 5});
    if (checked.status != HL_STATUS_OK || checked.value != 5) goto close_handles;
    memset(&binding, 0, sizeof(binding));
    binding.abi = HL_ENGINE_ABI;
    binding.size = sizeof(binding);
    binding.guest_fd = 10;
    binding.status_flags = HL_LINUX_O_RDONLY | HL_LINUX_O_DIRECTORY;
    binding.ownership = HL_ENGINE_FD_BORROW;
    binding.host_handle = directory.value;
    memset(&config, 0, sizeof(config));
    config.abi = HL_ENGINE_ABI;
    config.size = sizeof(config);
    config.guest_isa = HL_TEST_GUEST_ISA;
    config.fd_bindings = &binding;
    config.fd_binding_count = 1;
    if (hl_engine_create(&config, &services, &engine) != HL_STATUS_OK) goto close_handles;
    memset(&result, 0, sizeof(result));
    result.abi = HL_ENGINE_ABI;
    result.size = sizeof(result);
    if (hl_engine_run(engine, 1, (const char *const *)(argv + 1), &result) != HL_STATUS_OK ||
        result.kind != HL_ENGINE_EXIT_CODE || result.guest_status != 0)
        goto destroy;
    output = services.file->open_relative(services.context, directory.value, "output", 6, HL_HOST_FILE_READ, 0, 0);
    if (output.status != HL_STATUS_OK) goto destroy;
    checked = services.file->read_at(services.context, output.value, 0, (hl_host_bytes){bytes, 5});
    if (checked.status != HL_STATUS_OK || checked.value != 5 || memcmp(bytes, "mIXe!", 5)) goto destroy;
    outcome = 0;
destroy:
    hl_engine_destroy(engine);
close_handles:
    if (output.status == HL_STATUS_OK) (void)services.file->close(services.context, output.value);
    if (input.status == HL_STATUS_OK) (void)services.file->close(services.context, input.value);
    if (directory.status == HL_STATUS_OK) (void)services.file->close(services.context, directory.value);
    {
        char path[sizeof(directory_path) + 8];
        snprintf(path, sizeof(path), "%s/input", directory_path);
        unlink(path);
        snprintf(path, sizeof(path), "%s/output", directory_path);
        unlink(path);
    }
remove_directory:
    rmdir(directory_path);
done:
    hl_host_macos_destroy(host);
    return outcome;
}
