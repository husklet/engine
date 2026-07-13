#include "test.h"

#include "hl/engine.h"
#include "hl/fake_host.h"

#include <string.h>

int main(void) {
    hl_fake_host fake;
    hl_host_services services;
    hl_engine_config config;
    hl_engine_exit engine_exit;
    hl_engine *engine = NULL;

    hl_fake_host_init(&fake, &services);
    memset(&config, 0, sizeof(config));
    config.abi = HL_ENGINE_ABI;
    config.size = sizeof(config);
    config.guest_isa = HL_GUEST_ISA_AARCH64;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_OK && engine != NULL);
    memset(&engine_exit, 0, sizeof(engine_exit));
    engine_exit.abi = HL_ENGINE_ABI;
    engine_exit.size = sizeof(engine_exit);
    HL_CHECK(hl_engine_run(engine, 0, NULL, &engine_exit) == HL_STATUS_NOT_SUPPORTED);
    HL_CHECK(engine_exit.kind == HL_ENGINE_EXIT_ENGINE_ERROR);
    hl_engine_destroy(engine);

    config.abi++;
    engine = (hl_engine *)(uintptr_t)1;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_ABI_MISMATCH);
    HL_CHECK(engine == NULL);
    return EXIT_SUCCESS;
}
