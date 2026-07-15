#include "hl/activation.h"

#include <stdio.h>

int main(void) {
    hl_activation_process *process = NULL;
    hl_engine_exit result = {.abi = HL_ENGINE_ABI, .size = sizeof(result)};
    uint32_t ready = 0;
    uint64_t process_id = 0;

    if (hl_activation_start(NULL, HL_GUEST_ISA_AARCH64, NULL, &process) != HL_STATUS_INVALID_ARGUMENT ||
        process != NULL || hl_activation_process_id(NULL, &process_id) != HL_STATUS_INVALID_ARGUMENT ||
        hl_activation_wait(NULL, &result) != HL_STATUS_INVALID_ARGUMENT ||
        hl_activation_try_wait(NULL, &ready, &result) != HL_STATUS_INVALID_ARGUMENT ||
        hl_activation_kill(NULL) != HL_STATUS_INVALID_ARGUMENT ||
        hl_activation_spawn(NULL, HL_GUEST_ISA_AARCH64, NULL, NULL) != HL_STATUS_INVALID_ARGUMENT)
        return 1;
    hl_activation_process_destroy(NULL);
    puts("installed hl-engine activation package: ok");
    return 0;
}
