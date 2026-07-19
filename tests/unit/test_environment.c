#define _POSIX_C_SOURCE 200809L

#include "test.h"

#include <stdlib.h>
#include <string.h>

#include "../../src/core/environment.h"

int main(void) {
    long descriptor = -1;
    unsetenv("HL_LOG");
    unsetenv("HL_ACTIVATION_FD");
    HL_CHECK(hl_environment_debug_log() == NULL);
    HL_CHECK(hl_environment_take_activation_descriptor(&descriptor) == 0);
    HL_CHECK(setenv("HL_LOG", "syscall", 1) == 0);
    HL_CHECK(strcmp(hl_environment_debug_log(), "syscall") == 0);
    HL_CHECK(setenv("HL_ACTIVATION_FD", "198", 1) == 0);
    HL_CHECK(hl_environment_take_activation_descriptor(&descriptor) == 1);
    HL_CHECK(descriptor == 198 && getenv("HL_ACTIVATION_FD") == NULL);
    HL_CHECK(setenv("HL_ACTIVATION_FD", "not-a-descriptor", 1) == 0);
    HL_CHECK(hl_environment_take_activation_descriptor(&descriptor) == -1);
    HL_CHECK(getenv("HL_ACTIVATION_FD") == NULL);
    unsetenv("HL_LOG");
    return EXIT_SUCCESS;
}
