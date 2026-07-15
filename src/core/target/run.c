#include "native.h"

#include "hl/linux_abi.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

int hl_native_result_store(const hl_host_services *services, const char *path, const hl_launch_result *result) {
    hl_host_result opened;
    size_t offset = 0;
    int failed = 0;
    if (services == NULL || services->file == NULL || path == NULL || path[0] == '\0') return -1;
    opened = services->file->open_relative(services->context, HL_HOST_HANDLE_CWD, path, strlen(path),
                                           HL_HOST_FILE_WRITE | HL_HOST_FILE_NOFOLLOW, HL_HOST_FILE_TRUNCATE, 0);
    if (opened.status != HL_STATUS_OK) return -1;
    while (offset < sizeof(*result)) {
        hl_host_result written =
            services->file->write(services->context, opened.value, (const unsigned char *)result + offset,
                                  (uint64_t)(sizeof(*result) - offset));
        if (written.status != HL_STATUS_OK || written.value == 0 || written.value > sizeof(*result) - offset) {
            failed = 1;
            break;
        }
        offset += (size_t)written.value;
    }
    if (!failed && services->file->sync(services->context, opened.value).status != HL_STATUS_OK) failed = 1;
    if (services->file->close(services->context, opened.value).status != HL_STATUS_OK) failed = 1;
    return failed ? -1 : 0;
}

int hl_native_engine_run(uint32_t guest_isa, const char *rootfs, uint32_t argc, char *const argv[],
                         const hl_options *options, const char *result_path) {
    hl_native_host *native = NULL;
    hl_host_services services = {0};
    hl_engine_fd_binding bindings[3] = {0};
    hl_engine_config config = {.abi = HL_ENGINE_ABI, .size = sizeof(config), .guest_isa = guest_isa, .rootfs = rootfs};
    hl_engine_exit result = {.abi = HL_ENGINE_ABI, .size = sizeof(result)};
    hl_engine *engine = NULL;
    hl_status status = hl_native_host_create(&native, &services);
    uint32_t binding_count = 0;
    int exit_status = 70;
    if (status == HL_STATUS_OK) {
        uint32_t stream;
        for (stream = 0; stream < 3; ++stream) {
            hl_host_result adopted = services.file->standard_stream(services.context, stream);
            uint32_t access;
            if (adopted.status == HL_STATUS_NOT_FOUND) continue;
            if (adopted.status != HL_STATUS_OK) {
                status = (hl_status)adopted.status;
                break;
            }
            access = (uint32_t)adopted.detail & (HL_HOST_FILE_READ | HL_HOST_FILE_WRITE);
            bindings[binding_count] = (hl_engine_fd_binding){
                .abi = HL_ENGINE_ABI,
                .size = sizeof(bindings[0]),
                .guest_fd = stream,
                .status_flags = access == (HL_HOST_FILE_READ | HL_HOST_FILE_WRITE) ? HL_LINUX_O_RDWR
                                : access == HL_HOST_FILE_WRITE                     ? HL_LINUX_O_WRONLY
                                                                                   : HL_LINUX_O_RDONLY,
                .ownership = HL_ENGINE_FD_TRANSFER,
                .host_handle = adopted.value};
            if (((uint32_t)adopted.detail & HL_HOST_FILE_APPEND) != 0)
                bindings[binding_count].status_flags |= HL_LINUX_O_APPEND;
            if (((uint32_t)adopted.detail & HL_HOST_FILE_NONBLOCK) != 0)
                bindings[binding_count].status_flags |= HL_LINUX_O_NONBLOCK;
            ++binding_count;
        }
        config.fd_bindings = bindings;
        config.fd_binding_count = binding_count;
    }
    if (status == HL_STATUS_OK) status = hl_engine_create_with_options(&config, &services, options, &engine);
    if (status != HL_STATUS_OK && services.file != NULL) {
        uint32_t index;
        for (index = 0; index < binding_count; ++index)
            (void)services.file->close(services.context, bindings[index].host_handle);
    }
    if (status == HL_STATUS_OK)
        status = hl_engine_run(engine, (int)argc, (const char *const *)(uintptr_t)argv, &result);
    if (status == HL_STATUS_OK && result.kind == HL_ENGINE_EXIT_CODE)
        exit_status = result.guest_status;
    else if (status == HL_STATUS_OK && result.kind == HL_ENGINE_EXIT_SIGNAL)
        exit_status = 128 + result.guest_status;
    else
        fprintf(stderr, "hl-engine: execution failed status=%d kind=%u guest=%d detail=%llu\n", (int)status,
                result.kind, result.guest_status, (unsigned long long)result.detail);
    hl_engine_destroy(engine);
    if (result_path != NULL) {
        hl_launch_result launch_result = {.magic = HL_LAUNCH_RESULT_MAGIC,
                                          .abi = HL_LAUNCH_RESULT_ABI,
                                          .kind = status == HL_STATUS_OK ? result.kind : HL_LAUNCH_RESULT_ENGINE_ERROR,
                                          .guest_status = status == HL_STATUS_OK ? result.guest_status : 0,
                                          .engine_status = (int32_t)status,
                                          .detail = status == HL_STATUS_OK ? result.detail : 0};
        exit_status = hl_native_result_store(&services, result_path, &launch_result) == 0 ? 0 : 78;
    }
    hl_native_host_destroy(native);
    return exit_status;
}
