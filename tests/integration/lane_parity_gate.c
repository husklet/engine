#define _POSIX_C_SOURCE 200809L

#include "lane_parity_gate.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            fprintf(stderr, "lane-parity test failed at line %d: %s\n", __LINE__, #condition);                         \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static int check(const char *manifest, const char *os, const char *cpu) {
    char path[] = "/tmp/hl-lane-parity.XXXXXX";
    int descriptor = mkstemp(path);
    if (descriptor < 0) return -1;
    size_t size = strlen(manifest);
    size_t offset = 0;
    while (offset < size) {
        ssize_t written = write(descriptor, manifest + offset, size - offset);
        if (written <= 0) {
            close(descriptor);
            unlink(path);
            return -1;
        }
        offset += (size_t)written;
    }
    if (close(descriptor) != 0) {
        unlink(path);
        return -1;
    }
    FILE *diagnostics = tmpfile();
    if (diagnostics == NULL) {
        unlink(path);
        return -1;
    }
    int result = hl_lane_parity_check(path, os, cpu, diagnostics);
    fclose(diagnostics);
    unlink(path);
    return result;
}

int main(void) {
    static const char base[] = "host\tLinux-aarch64\n"
                               "host\tLinux-x86_64\n"
                               "host\tDarwin-aarch64\n"
                               "host\tWindows-x86_64\n"
                               "reserve\tLinux-x86_64\temulated\n"
                               "reserve\tLinux-aarch64\tshared-reserved\n"
                               "reserve\tLinux-x86_64\tshared-reserved\n"
                               "lane\tLinux\tunit\n"
                               "lane\tLinux\temulated\n"
                               "lane\tLinux\tshared-reserved\n"
                               "lane\tDarwin\tmacos\n"
                               "lane\tWindows\tunit\n"
                               "test\tlinux-unit\tunit\n"
                               "test\temulated-case\temulated\n"
                               "test\tshared-case\tshared-reserved\n"
                               "test\tmac-case\tmacos\n"
                               "test\twindows-unit\tunit\n";
    CHECK(check(base, "Linux", "aarch64") == 0);
    CHECK(check(base, "Linux", "x86_64") == 0);
    CHECK(check(base, "Darwin", "aarch64") == 0);
    CHECK(check(base, "Windows", "x86_64") == 0);
    CHECK(check(base, "Plan9", "x86_64") == 2);
    CHECK(check(base, "Linux", "riscv64") == 2);

    static const char exact[] = "host\tLinux-aarch64\n"
                                "lane\tLinux\tunit\n"
                                "test\talmost\tunit-extra\n";
    CHECK(check(exact, "Linux", "aarch64") == 1);

    static const char self_only[] = "host\tLinux-aarch64\n"
                                    "lane\tLinux\tunit\n";
    CHECK(check(self_only, "Linux", "aarch64") == 1);

    static const char empty[] = "host\tLinux-aarch64\n"
                                "lane\tDarwin\tmacos\n"
                                "test\tmac\tmacos\n";
    CHECK(check(empty, "Linux", "aarch64") == 1);

    static const char undeclared_reservation[] = "host\tLinux-aarch64\n"
                                                 "reserve\tLinux-x86_64\tunit\n"
                                                 "lane\tLinux\tunit\n"
                                                 "test\tcase\tunit\n";
    CHECK(check(undeclared_reservation, "Linux", "aarch64") == 2);

    static const char duplicate[] = "host\tLinux-aarch64\n"
                                    "host\tLinux-aarch64\n"
                                    "lane\tLinux\tunit\n"
                                    "test\tcase\tunit\n";
    CHECK(check(duplicate, "Linux", "aarch64") == 2);
    CHECK(check("host\tLinux-aarch64\nlane\tLinux\tunit\nlane\tLinux\tunit\ntest\tcase\tunit\n", "Linux", "aarch64") ==
          2);
    CHECK(check("host\tLinux-aarch64\nreserve\tLinux-aarch64\nlane\tLinux\tunit\ntest\tcase\tunit\n", "Linux",
                "aarch64") == 2);
    CHECK(check("host\tLinux-aarch64", "Linux", "aarch64") == 2);
    CHECK(check("host\tLinux/aarch64\n", "Linux", "aarch64") == 2);
    CHECK(check("unknown\tvalue\n", "Linux", "aarch64") == 2);
    return 0;
}
