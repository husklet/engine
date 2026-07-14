#include "hl/engine.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("hl-engine %s abi=%u\n", hl_engine_version(), hl_engine_abi());
        return 0;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: %s ENGINE GUEST [args...]\n", argv[0]);
        return 64;
    }
    execv(argv[1], argv + 1);
    perror("hl-engine-runner: execv");
    return 126;
}
