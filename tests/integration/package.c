#include <hl/engine.h>

#if defined(__APPLE__)
#include <hl/macos.h>
typedef hl_host_macos package_host;
#define package_host_create hl_host_macos_create
#define package_host_destroy hl_host_macos_destroy
#elif defined(__linux__)
#include <hl/linux.h>
typedef hl_host_linux package_host;
#define package_host_create hl_host_linux_create
#define package_host_destroy hl_host_linux_destroy
#else
#error "the package integration test needs a supported host provider"
#endif

#include <stdio.h>
#include <string.h>

int main(void) {
    package_host *host = NULL;
    hl_host_services services = {0};

    if (hl_engine_abi() != HL_ENGINE_ABI || strcmp(hl_engine_version(), "0.1.0") != 0) {
        return 1;
    }
    if (package_host_create(&host, &services) != HL_STATUS_OK || host == NULL) {
        return 2;
    }
    if (hl_host_services_validate(&services, HL_HOST_CAP_MEMORY | HL_HOST_CAP_FILE | HL_HOST_CAP_CLOCK) !=
        HL_STATUS_OK) {
        package_host_destroy(host);
        return 3;
    }
    package_host_destroy(host);
    puts("installed hl-engine package: ok");
    return 0;
}
