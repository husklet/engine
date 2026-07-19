#define _POSIX_C_SOURCE 200809L

#include "test.h"

#include <stdlib.h>
#include <string.h>

#include "../../src/core/options.h"

int main(void) {
    hl_options options;

    HL_CHECK(setenv("HL_LOG", "fs", 1) == 0);
    HL_CHECK(hl_options_init(&options) == 0);
    hl_options_import_environment(&options);
    HL_CHECK(strcmp(hl_options_get(&options, "HL_LOG"), "fs") == 0);

    HL_CHECK(hl_options_set(&options, "HL_LOG", "network", 1) == 0);
    HL_CHECK(setenv("HL_LOG", "syscall", 1) == 0);
    hl_options_import_environment(&options);
    HL_CHECK(strcmp(hl_options_get(&options, "HL_LOG"), "network") == 0);

    hl_options_destroy(&options);
    unsetenv("HL_LOG");
    return EXIT_SUCCESS;
}
