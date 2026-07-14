#include "test.h"

#include "hl/fake_host.h"

#include <string.h>

static int32_t fake_process_entry(void *context) {
    return context == NULL ? 23 : 24;
}

int main(void) {
    hl_fake_host fake;
    hl_host_services services;
    hl_host_result mapping;
    hl_host_services truncated;
    hl_host_result process;
    hl_host_result process_exit;

    hl_fake_host_init(&fake, &services);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK) == HL_STATUS_OK);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_GPU) == HL_STATUS_NOT_SUPPORTED);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_PROCESS) == HL_STATUS_OK);

    truncated = services;
    truncated.size = 8;
    HL_CHECK(hl_host_services_validate(&truncated, 0) == HL_STATUS_ABI_MISMATCH);

    mapping = services.memory->reserve(services.context, 4096, 4096, 0);
    HL_CHECK(mapping.status == HL_STATUS_OK && mapping.value != 0 && fake.live_mappings == 1);
    HL_CHECK(services.memory->publish_code(services.context, mapping.value, 0, 4096).status == HL_STATUS_OK);
    HL_CHECK(services.memory->release(services.context, mapping.value).status == HL_STATUS_OK);
    HL_CHECK(fake.live_mappings == 0);

    process = services.process->spawn_cloned(services.context, fake_process_entry, NULL);
    HL_CHECK(process.status == HL_STATUS_OK && process.value != 0 && fake.live_processes == 1);
    HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_BUSY);
    fake.process_exit_value = 37;
    process_exit = services.process->wait(services.context, process.value, HL_HOST_DEADLINE_INFINITE);
    HL_CHECK(process_exit.status == HL_STATUS_OK && process_exit.detail == HL_HOST_PROCESS_EXIT_CODE &&
             process_exit.value == 37);
    HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_OK);
    HL_CHECK(fake.live_processes == 0);

    hl_fake_host_fail_next(&fake, HL_STATUS_OUT_OF_MEMORY);
    HL_CHECK(services.memory->reserve(services.context, 4096, 4096, 0).status == HL_STATUS_OUT_OF_MEMORY);
    HL_CHECK(fake.live_mappings == 0);
    return EXIT_SUCCESS;
}
