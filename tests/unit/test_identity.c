#include "test.h"

#include "../../src/translator/identity.h"

int main(void) {
    uint64_t busybox = hl_identity_name("busybox");
    hl_host_file_metadata file = {.stable_device = 3,
                                  .stable_object = 5,
                                  .size = 7,
                                  .modified_ns = UINT64_C(11000000013),
                                  .type = HL_HOST_FILE_TYPE_REGULAR};
    uint64_t expected = UINT64_C(1469598103934665603);
    const uint64_t fields[] = {3, 5, 7, 11, 13};

    HL_CHECK(hl_identity_name(NULL) == 0x1357ull);
    HL_CHECK(hl_identity_name("") == 1469598103934665603ull);
    HL_CHECK(hl_identity_name("/bin/busybox") == busybox);
    HL_CHECK(hl_identity_name("/usr/local/bin/busybox") == busybox);
    HL_CHECK(hl_identity_name("/bin/sh") != busybox);
    HL_CHECK(hl_identity_mix(1, 2, 3, 4) == ((1ull ^ (2ull * 1099511628211ull)) ^ 3ull ^ (4ull * 0x100000001B3ull)));
    for (size_t index = 0; index < HL_ARRAY_COUNT(fields); ++index) {
        expected ^= fields[index];
        expected *= UINT64_C(1099511628211);
    }
    HL_CHECK(hl_identity_file(&file) == expected);
    file.type = HL_HOST_FILE_TYPE_DIRECTORY;
    HL_CHECK(hl_identity_file(&file) == 0);
    HL_CHECK(hl_identity_file(NULL) == 0);
    return EXIT_SUCCESS;
}
