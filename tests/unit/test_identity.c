#include "test.h"

#include "../../src/translator/identity.h"

int main(void) {
    uint64_t busybox = hl_identity_name("busybox");

    HL_CHECK(hl_identity_name(NULL) == 0x1357ull);
    HL_CHECK(hl_identity_name("") == 1469598103934665603ull);
    HL_CHECK(hl_identity_name("/bin/busybox") == busybox);
    HL_CHECK(hl_identity_name("/usr/local/bin/busybox") == busybox);
    HL_CHECK(hl_identity_name("/bin/sh") != busybox);
    HL_CHECK(hl_identity_mix(1, 2, 3, 4) == ((1ull ^ (2ull * 1099511628211ull)) ^ 3ull ^ (4ull * 0x100000001B3ull)));
    return EXIT_SUCCESS;
}
