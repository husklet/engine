#include "hl/engine.h"
#include "hl/macos.h"

#include <stdio.h>
#include <string.h>

static int run_one(const hl_host_services *services, uint32_t guest_isa, const char *guest) {
    hl_engine_config config = {0};
    hl_engine_exit result = {0};
    hl_engine *engine = NULL;
    const char *argv[] = {guest};
    hl_status status;
    config.abi = HL_ENGINE_ABI;
    config.size = sizeof(config);
    config.guest_isa = guest_isa;
    result.abi = HL_ENGINE_ABI;
    result.size = sizeof(result);
    status = hl_engine_create(&config, services, &engine);
    if (status == HL_STATUS_OK) status = hl_engine_run(engine, 1, argv, &result);
    hl_engine_destroy(engine);
    if (status != HL_STATUS_OK || result.kind != HL_ENGINE_EXIT_CODE || result.guest_status != 42) {
        fprintf(stderr, "dual backend: isa=%u status=%d kind=%u guest=%d detail=%llu\n", guest_isa,
                status, result.kind, result.guest_status, (unsigned long long)result.detail);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    hl_host_macos *host = NULL;
    hl_host_services services;
    int failed;
    if (argc != 3) {
        fprintf(stderr, "usage: dual-backend-e2e AARCH64_GUEST X86_64_GUEST\n");
        return 64;
    }
    if (hl_host_macos_create(&host, &services) != HL_STATUS_OK) return 70;
    failed = run_one(&services, HL_GUEST_ISA_AARCH64, argv[1]);
    if (!failed) failed = run_one(&services, HL_GUEST_ISA_X86_64, argv[2]);
    hl_host_macos_destroy(host);
    return failed ? 71 : 0;
}
