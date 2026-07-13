#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const forbidden[] = {"<mach/",         "<sys/event.h>", "<libkern/", "IOSurface",
                                        "CoreFoundation", "windows.h",     "<linux/"};

static int check_file(const char *path) {
    FILE *file = fopen(path, "rb");
    char buffer[4096];
    unsigned long line = 0;
    int failed = 0;
    if (file == NULL) {
        perror(path);
        return 1;
    }
    while (fgets(buffer, sizeof(buffer), file) != NULL) {
        size_t i;
        line++;
        for (i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
            if (strstr(buffer, forbidden[i]) != NULL) {
                fprintf(stderr, "%s:%lu: portable domain contains forbidden token %s\n", path, line, forbidden[i]);
                failed = 1;
            }
        }
    }
    if (ferror(file)) failed = 1;
    fclose(file);
    return failed;
}

int main(int argc, char **argv) {
    int failed = 0;
    int i;
    if (argc < 2) {
        fprintf(stderr, "usage: check-domains FILE...\n");
        return 2;
    }
    for (i = 1; i < argc; ++i)
        failed |= check_file(argv[i]);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
