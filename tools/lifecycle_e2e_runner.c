#include "hl/engine.h"
#include "hl/host_macos.h"

#include <stdio.h>
#include <string.h>

#ifndef HL_TEST_GUEST_ISA
#error HL_TEST_GUEST_ISA is required
#endif

int main(int argc, char **argv) {
    hl_host_macos *host = NULL;
    hl_host_services services;
    hl_engine_config config;
    hl_engine_exit result;
    hl_engine *engine = NULL;
    hl_status status;
    if (argc < 2) {
        fprintf(stderr, "usage: lifecycle-e2e-runner GUEST [args...]\n");
        return 64;
    }
    status = hl_host_macos_create(&host, &services);
    if (status != HL_STATUS_OK) return 70;
    memset(&config, 0, sizeof config);
    config.abi = HL_ENGINE_ABI;
    config.size = sizeof config;
    config.guest_isa = HL_TEST_GUEST_ISA;
    status = hl_engine_create(&config, &services, &engine);
    if (status != HL_STATUS_OK) return 71;
    memset(&result, 0, sizeof result);
    result.abi = HL_ENGINE_ABI;
    result.size = sizeof result;
    status = hl_engine_run(engine, argc - 1, (const char *const *)(argv + 1), &result);
    hl_engine_destroy(engine);
    hl_host_macos_destroy(host);
    if (status != HL_STATUS_OK || result.kind != HL_ENGINE_EXIT_CODE) return 72;
    return result.guest_status;
}
