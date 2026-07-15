#include "hl/engine.h"
#include "hl/linux_abi.h"
#include "hl/macos.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef HL_TEST_GUEST_ISA
#error HL_TEST_GUEST_ISA is required
#endif

int main(int argc, char **argv) {
    hl_host_macos *host = NULL;
    hl_host_services services;
    hl_engine_fd_binding bindings[2];
    hl_engine_config config;
    hl_engine_exit result;
    hl_engine *engine = NULL;
    hl_host_result opened;
    hl_host_result checked;
    char path[160];
    char bytes[6];
    hl_status status;
    int outcome = 1;
    if (argc != 2) return 64;
    if (hl_host_macos_create(&host, &services) != HL_STATUS_OK) return 70;
    snprintf(path, sizeof(path), "/tmp/hl-binding-%ld", (long)getpid());
    opened = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, path, strlen(path),
                                          HL_HOST_FILE_READ | HL_HOST_FILE_WRITE,
                                          HL_HOST_FILE_CREATE | HL_HOST_FILE_TRUNCATE, 0600);
    if (opened.status != HL_STATUS_OK) goto done;
    checked = services.file->write_at(services.context, opened.value, 0, (hl_host_const_bytes){"abcd", 4});
    if (checked.status != HL_STATUS_OK || checked.value != 4) goto close_file;
    memset(bindings, 0, sizeof(bindings));
    bindings[0].abi = HL_ENGINE_ABI;
    bindings[0].size = sizeof(bindings[0]);
    bindings[0].guest_fd = 100;
    bindings[0].status_flags = HL_LINUX_O_RDWR;
    bindings[0].ownership = HL_ENGINE_FD_BORROW;
    bindings[0].host_handle = opened.value;
    bindings[1] = bindings[0];
    bindings[1].guest_fd = 700;
    bindings[1].descriptor_flags = HL_LINUX_FD_CLOEXEC;
    memset(&config, 0, sizeof(config));
    config.abi = HL_ENGINE_ABI;
    config.size = sizeof(config);
    config.guest_isa = HL_TEST_GUEST_ISA;
    config.fd_bindings = bindings;
    config.fd_binding_count = 2;
    status = hl_engine_create(&config, &services, &engine);
    if (status != HL_STATUS_OK) goto close_file;
    memset(&result, 0, sizeof(result));
    result.abi = HL_ENGINE_ABI;
    result.size = sizeof(result);
    status = hl_engine_run(engine, 1, (const char *const *)(argv + 1), &result);
    if (status != HL_STATUS_OK || result.kind != HL_ENGINE_EXIT_CODE || result.guest_status != 0) {
        fprintf(stderr, "binding: run status=%d kind=%u guest=%d\n", status, result.kind, result.guest_status);
        goto destroy;
    }
    checked = services.file->read_at(services.context, opened.value, 0, (hl_host_bytes){bytes, sizeof(bytes)});
    if (checked.status != HL_STATUS_OK || checked.value != sizeof(bytes) || memcmp(bytes, "XbcPCD", sizeof(bytes))) {
        fprintf(stderr, "binding: verify status=%d size=%llu bytes=%.*s\n", checked.status,
                (unsigned long long)checked.value, (int)sizeof(bytes), bytes);
        goto destroy;
    }
    outcome = 0;
destroy:
    hl_engine_destroy(engine);
close_file:
    (void)services.file->close(services.context, opened.value);
    unlink(path);
done:
    hl_host_macos_destroy(host);
    return outcome;
}
