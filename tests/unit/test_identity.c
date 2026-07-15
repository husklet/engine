#include "test.h"

#include "../../src/translator/identity.h"

#include <string.h>

typedef struct identity_fake { hl_host_file_metadata metadata; int closes; } identity_fake;

static hl_host_result identity_open(void *context, hl_host_handle directory, const char *path, size_t size,
                                    uint32_t access, uint32_t creation, uint32_t permissions) {
    (void)context; (void)creation; (void)permissions;
    return directory == HL_HOST_HANDLE_CWD && size == 4 && memcmp(path, "/bin", 4) == 0 &&
                   access == (HL_HOST_FILE_READ | HL_HOST_FILE_NOFOLLOW)
               ? (hl_host_result){HL_STATUS_OK, 0, 7, 0}
               : (hl_host_result){HL_STATUS_NOT_FOUND, 0, 0, 0};
}

static hl_host_result identity_metadata(void *context, hl_host_handle file, hl_host_file_metadata *output) {
    identity_fake *fake = context;
    if (file != 7) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    *output = fake->metadata;
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
}

static hl_host_result identity_close(void *context, hl_host_handle file) {
    identity_fake *fake = context;
    if (file != 7) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    fake->closes++;
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
}

int main(void) {
    uint64_t busybox = hl_identity_name("busybox");
    hl_host_file_metadata file = {.stable_device = 3,
                                  .stable_object = 5,
                                  .size = 7,
                                  .modified_ns = UINT64_C(11000000013),
                                  .type = HL_HOST_FILE_TYPE_REGULAR};
    uint64_t expected = UINT64_C(1469598103934665603);
    const uint64_t fields[] = {3, 5, 7, 11, 13};
    identity_fake fake = {.metadata = file};
    static const hl_host_file_services file_services = {
        .abi = HL_HOST_FILE_ABI, .size = sizeof(file_services), .open_relative = identity_open,
        .metadata = identity_metadata, .close = identity_close,
    };
    hl_host_services services = {
        .abi = HL_HOST_SERVICES_ABI, .size = sizeof(services), .context = &fake, .file = &file_services,
    };

    HL_CHECK(hl_identity_name(NULL) == 0x1357ull);
    HL_CHECK(hl_identity_name("") == 1469598103934665603ull);
    HL_CHECK(hl_identity_name("/bin/busybox") == busybox);
    HL_CHECK(hl_identity_name("/usr/local/bin/busybox") == busybox);
    HL_CHECK(hl_identity_name("/bin/sh") != busybox);
    HL_CHECK(hl_identity_mix(1, 2, 3, 4) == ((1ull ^ (2ull * 1099511628211ull)) ^ 3ull ^ (4ull * 0x100000001B3ull)));
    {
        uint64_t configuration = hl_identity_configuration(11, 1, 1, 0);
        HL_CHECK(hl_identity_configuration(12, 1, 1, 0) != configuration);
        HL_CHECK(hl_identity_configuration(11, 2, 1, 0) != configuration);
        HL_CHECK(hl_identity_configuration(11, 1, 2, 0) != configuration);
        HL_CHECK(hl_identity_configuration(11, 1, 1, 1) != configuration);
    }
    for (size_t index = 0; index < HL_ARRAY_COUNT(fields); ++index) {
        expected ^= fields[index];
        expected *= UINT64_C(1099511628211);
    }
    HL_CHECK(hl_identity_file(&file) == expected);
    uint64_t source = hl_identity_source(&services, "/bin");
    HL_CHECK(source != 0 && fake.closes == 1);
    fake.metadata.changed_ns++;
    HL_CHECK(hl_identity_source(&services, "/bin") != source && fake.closes == 2);
    HL_CHECK(hl_identity_source(&services, "/missing") == 0 && fake.closes == 2);
    file.type = HL_HOST_FILE_TYPE_DIRECTORY;
    HL_CHECK(hl_identity_file(&file) == 0);
    HL_CHECK(hl_identity_file(NULL) == 0);
    return EXIT_SUCCESS;
}
