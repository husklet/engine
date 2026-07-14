#include "test.h"

#include "../../src/core/target/native.h"

#include <string.h>

int main(void) {
    hl_native_host *host = NULL;
    hl_host_services services = {0};
    hl_host_services injected;
    HL_CHECK(HL_NATIVE_HOST_NAME[0] != '\0');
    HL_CHECK(strcmp(HL_NATIVE_HOST_NAME, "macos") == 0 || strcmp(HL_NATIVE_HOST_NAME, "linux") == 0);
    HL_CHECK(hl_native_host_bind(&host, &services, NULL) == 0);
    HL_CHECK(host != NULL);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK | HL_HOST_CAP_CODE_MAPPING) ==
             HL_STATUS_OK);
    injected = services;
    HL_CHECK(hl_native_host_bind(&host, &services, &injected) == 0);
    injected.context = NULL;
    HL_CHECK(hl_native_host_bind(&host, &services, &injected) == -1);
    HL_CHECK(hl_native_host_bind(&host, &services, NULL) == 0);
    hl_native_host_destroy(host);
    return 0;
}
