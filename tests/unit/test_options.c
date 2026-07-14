#define _POSIX_C_SOURCE 200809L

#include "test.h"

#include <stdlib.h>
#include <string.h>

#include "../../src/core/options.h"

int main(void) {
    char mutable[] = "original";

    hl_option_reset();
    HL_CHECK(setenv("HL_CWD", "/ambient", 1) == 0);
    HL_CHECK(setenv("MAPDUMP", "/tmp/legacy-mapdump", 1) == 0);
    HL_CHECK(hl_option_get("HL_CWD") == NULL);
    HL_CHECK(hl_option_get("MAPDUMP") == NULL);
    HL_CHECK(hl_option_set("MAPDUMP", "value", 1) == -1);
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
    HL_CHECK(hl_option_get("HL_CWD") == NULL);
    unsetenv("HL_CWD");
    unsetenv("MAPDUMP");
    return EXIT_SUCCESS;
}
