#define _POSIX_C_SOURCE 200809L

#include "test.h"

#include <stdlib.h>
#include <string.h>

#include "../../src/production/engine/options.c"

int main(void) {
    char mutable[] = "original";

    hl_option_reset();
    HL_CHECK(setenv("HL_CWD", "/ambient", 1) == 0);
    HL_CHECK(hl_option_get("HL_CWD") == NULL);
    HL_CHECK(hl_option_set("HL_CWD", mutable, 1) == 0);
    mutable[0] = 'X';
    HL_CHECK(strcmp(hl_option_get("HL_CWD"), "original") == 0);
    HL_CHECK(hl_option_set("HL_CWD", "ignored", 0) == 0);
    HL_CHECK(strcmp(hl_option_get("HL_CWD"), "original") == 0);
    HL_CHECK(hl_option_set("HL_CWD", "replacement", 1) == 0);
    HL_CHECK(strcmp(hl_option_get("HL_CWD"), "replacement") == 0);
    HL_CHECK(hl_option_unset("HL_CWD") == 0 && hl_option_get("HL_CWD") == NULL);
    HL_CHECK(hl_option_set("HL_NOT_REGISTERED", "value", 1) == -1);
    HL_CHECK(hl_option_unset("HL_NOT_REGISTERED") == -1);
    hl_option_reset();
    HL_CHECK(hl_option_get("HL_CWD") == NULL && hl_option_store_size == 0);
    unsetenv("HL_CWD");
    return EXIT_SUCCESS;
}
