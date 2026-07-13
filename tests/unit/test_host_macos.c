#include "test.h"

#include "hl/host_macos.h"

#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    hl_host_macos *host;
    hl_host_services services;
    hl_host_code_mapping code;
    HL_CHECK(hl_host_macos_create(&host, &services) == HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK | HL_HOST_CAP_CODE_MAPPING) ==
             HL_STATUS_OK);
    HL_CHECK(services.clock->monotonic_ns(services.context).status == HL_STATUS_OK);
    memset(&code, 0, sizeof code);
    HL_CHECK(services.memory->reserve_code(services.context, 16384, 16384, &code).status == HL_STATUS_OK);
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
