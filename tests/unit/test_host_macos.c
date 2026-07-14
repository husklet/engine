#include "test.h"

#include "hl/host_macos.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    hl_host_macos *host;
    hl_host_services services;
    hl_host_code_mapping code;
    hl_host_result file;
    char path[128];
    char contents[3] = {0};
    HL_CHECK(hl_host_macos_create(&host, &services) == HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK | HL_HOST_CAP_CODE_MAPPING) ==
             HL_STATUS_OK);
    HL_CHECK(services.clock->monotonic_ns(services.context).status == HL_STATUS_OK);
    snprintf(path, sizeof(path), "/tmp/hl_host_macos_%ld", (long)getpid());
    file = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, path, strlen(path),
                                        HL_HOST_FILE_READ | HL_HOST_FILE_WRITE | HL_HOST_FILE_APPEND,
                                        HL_HOST_FILE_CREATE | HL_HOST_FILE_EXCLUSIVE, 0600);
    HL_CHECK(file.status == HL_STATUS_OK);
    HL_CHECK(services.file->write_at(services.context, file.value, 0, (hl_host_const_bytes){"a", 1}).value == 1);
    HL_CHECK(services.file->append(services.context, file.value, (hl_host_const_bytes){"bc", 2}).value == 2);
    HL_CHECK(
        services.file->read_at(services.context, file.value, 0, (hl_host_bytes){contents, sizeof(contents)}).value ==
        sizeof(contents));
    HL_CHECK(memcmp(contents, "abc", sizeof(contents)) == 0);
    HL_CHECK(services.file->close(services.context, file.value).status == HL_STATUS_OK);
    unlink(path);
    memset(&code, 0, sizeof code);
    HL_CHECK(services.memory->reserve_code(services.context, 16384, 16384, HL_HOST_CODE_DUAL_ALIAS, &code).status ==
             HL_STATUS_OK);
    memcpy((void *)(uintptr_t)code.writable_address, "code", 5);
    HL_CHECK(services.memory->publish_code(services.context, code.handle, 0, 5).status == HL_STATUS_OK);
    HL_CHECK(memcmp((const void *)(uintptr_t)code.executable_address, "code", 5) == 0);
    pid_t child = fork();
    HL_CHECK(child >= 0);
    if (child == 0) {
        if (services.memory->repair_code_after_fork(services.context, &code, 1).status != HL_STATUS_OK) _exit(10);
        memcpy((void *)(uintptr_t)code.writable_address, "fork", 5);
        if (services.memory->publish_code(services.context, code.handle, 0, 5).status != HL_STATUS_OK) _exit(11);
        _exit(memcmp((const void *)(uintptr_t)code.executable_address, "fork", 5) == 0 ? 0 : 12);
    }
    int status = 0;
    HL_CHECK(waitpid(child, &status, 0) == child);
    HL_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    HL_CHECK(services.memory->release(services.context, code.handle).status == HL_STATUS_OK);
    hl_host_macos_destroy(host);
    return EXIT_SUCCESS;
}
