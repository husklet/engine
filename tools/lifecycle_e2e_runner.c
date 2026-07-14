#include "hl/engine.h"
#include "hl/host_macos.h"

#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>

#ifndef HL_TEST_GUEST_ISA
#error HL_TEST_GUEST_ISA is required
#endif

typedef struct lifecycle_run {
    hl_engine *engine;
    int argc;
    const char *const *argv;
    hl_engine_exit *result;
    hl_status status;
} lifecycle_run;

static void *run_guest(void *opaque) {
    lifecycle_run *run = opaque;
    run->status = hl_engine_run(run->engine, run->argc, run->argv, run->result);
    return NULL;
}

int main(int argc, char **argv) {
    hl_host_macos *host = NULL;
    hl_host_services services;
    hl_engine_config config;
    hl_engine_exit result;
    hl_engine *engine = NULL;
    hl_status status;
    int force_stop = argc > 1 && strcmp(argv[1], "--force-stop") == 0;
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
    if (force_stop) {
        lifecycle_run run = {engine, argc - 2, (const char *const *)(argv + 2), &result, HL_STATUS_BUSY};
        pthread_t thread;
        uint32_t attempts;
        if (argc < 3 || pthread_create(&thread, NULL, run_guest, &run) != 0) return 73;
        for (attempts = 0; attempts < UINT32_C(1000000); ++attempts) {
            status = hl_engine_request(engine, HL_ENGINE_REQUEST_FORCE_STOP, NULL, 0);
            if (status != HL_STATUS_BUSY) break;
            sched_yield();
        }
        if (status != HL_STATUS_OK || pthread_join(thread, NULL) != 0) return 74;
        status = run.status;
    } else {
        status = hl_engine_run(engine, argc - 1, (const char *const *)(argv + 1), &result);
    }
    hl_engine_destroy(engine);
    hl_host_macos_destroy(host);
    if (force_stop)
        return status == HL_STATUS_OK && result.kind == HL_ENGINE_EXIT_SIGNAL && result.guest_status == 9 ? 0 : 75;
    if (status != HL_STATUS_OK || result.kind != HL_ENGINE_EXIT_CODE) return 72;
    return result.guest_status;
}
