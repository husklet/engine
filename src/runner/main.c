#include "hl/engine.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("hl-engine %s abi=%u\n", hl_engine_version(), hl_engine_abi());
        return 0;
    }
    fprintf(stderr, "hl-engine-runner: execution adapter is not wired; use --version\n");
    return 64;
}
