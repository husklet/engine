#include "test.h"

#include "hl/engine.h"
#include "hl/fake_host.h"
#include "../../src/core/engine_backend.h"

#include <pthread.h>
#include <sched.h>
#include <string.h>

static int32_t fake_entry(void *context) {
    (void)context;
    return 0;
}

static hl_status fake_start(const hl_host_services *host, const char *rootfs, int argc, const char *const argv[],
                            hl_host_handle *process) {
    hl_host_result spawned;
    (void)rootfs;
    (void)argc;
    (void)argv;
    spawned = host->process->spawn_cloned(host->context, fake_entry, NULL);
    if (spawned.status != HL_STATUS_OK) return (hl_status)spawned.status;
    *process = spawned.value;
    return HL_STATUS_OK;
}

static const hl_engine_backend fake_backend = {HL_GUEST_ISA_AARCH64, fake_start};

typedef struct run_context {
    hl_engine *engine;
    hl_engine_exit result;
    hl_status status;
} run_context;

static void *run_engine(void *opaque) {
    run_context *context = opaque;
    context->status = hl_engine_run(context->engine, 0, NULL, &context->result);
    return NULL;
}

static int check_concurrent_stop(hl_fake_host *fake, hl_host_services *services, hl_engine_config *config,
                                 uint32_t request, int32_t expected_signal) {
    run_context context;
    pthread_t thread;
    memset(&context, 0, sizeof(context));
    context.result.abi = HL_ENGINE_ABI;
    context.result.size = sizeof(context.result);
    hl_engine_backend_register(&fake_backend);
    HL_CHECK(hl_engine_create(config, services, &context.engine) == HL_STATUS_OK);
    HL_CHECK(hl_engine_request(context.engine, request, NULL, 0) == HL_STATUS_BUSY);
    hl_fake_host_block_process_wait(fake, 1);
    HL_CHECK(pthread_create(&thread, NULL, run_engine, &context) == 0);
    while (__atomic_load_n(&fake->live_processes, __ATOMIC_ACQUIRE) == 0)
        sched_yield();
    HL_CHECK(hl_engine_request(context.engine, request, NULL, 0) == HL_STATUS_OK);
    HL_CHECK(pthread_join(thread, NULL) == 0);
    HL_CHECK(context.status == HL_STATUS_OK && context.result.kind == HL_ENGINE_EXIT_SIGNAL &&
             context.result.guest_status == expected_signal);
    HL_CHECK(hl_engine_request(context.engine, request, NULL, 0) == HL_STATUS_BUSY);
    HL_CHECK(hl_engine_run(context.engine, 0, NULL, &context.result) == HL_STATUS_BUSY);
    hl_engine_destroy(context.engine);
    return EXIT_SUCCESS;
}

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
    HL_CHECK(hl_engine_request(engine, UINT32_MAX, NULL, 0) == HL_STATUS_NOT_SUPPORTED);
    HL_CHECK(hl_engine_request(engine, HL_ENGINE_REQUEST_FORCE_STOP, "x", 1) == HL_STATUS_INVALID_ARGUMENT);
    memset(&engine_exit, 0, sizeof(engine_exit));
    engine_exit.abi = HL_ENGINE_ABI;
    engine_exit.size = sizeof(engine_exit);
    HL_CHECK(hl_engine_run(engine, 0, NULL, &engine_exit) == HL_STATUS_NOT_SUPPORTED);
    HL_CHECK(engine_exit.kind == HL_ENGINE_EXIT_ENGINE_ERROR);
    hl_engine_destroy(engine);

    HL_CHECK(check_concurrent_stop(&fake, &services, &config, HL_ENGINE_REQUEST_INTERRUPT, 2) == EXIT_SUCCESS);
    HL_CHECK(check_concurrent_stop(&fake, &services, &config, HL_ENGINE_REQUEST_FORCE_STOP, 9) == EXIT_SUCCESS);

    config.abi++;
    engine = (hl_engine *)(uintptr_t)1;
    HL_CHECK(hl_engine_create(&config, &services, &engine) == HL_STATUS_ABI_MISMATCH);
    HL_CHECK(engine == NULL);
    return EXIT_SUCCESS;
}
