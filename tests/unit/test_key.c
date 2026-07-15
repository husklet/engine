#include "test.h"

#include "../../src/linux_abi/container/key.h"

#include <string.h>

int main(void) {
    char output[17];
    char repeat[17];
    char punctuated[17];

    HL_CHECK(hl_linux_container_key("", output, sizeof output) == 0);
    HL_CHECK(strcmp(output, "cbf29ce484222325") == 0);
    HL_CHECK(hl_linux_container_key("shared-container", output, sizeof output) == 0);
    HL_CHECK(hl_linux_container_key("shared-container", repeat, sizeof repeat) == 0);
    HL_CHECK(strcmp(output, repeat) == 0);
    HL_CHECK(hl_linux_container_key("../shared/container", punctuated, sizeof punctuated) == 0);
    HL_CHECK(strcmp(output, punctuated) != 0);
    HL_CHECK(strlen(punctuated) == 16);
    HL_CHECK(strspn(punctuated, "0123456789abcdef") == 16);

    memset(output, 'x', sizeof output);
    HL_CHECK(hl_linux_container_key(NULL, output, sizeof output) == -1);
    HL_CHECK(hl_linux_container_key("key", NULL, sizeof output) == -1);
    HL_CHECK(hl_linux_container_key("key", output, sizeof output - 1) == -1);
    HL_CHECK(output[0] == 'x');
    return EXIT_SUCCESS;
}
