#include "test.h"

#include <stdlib.h>
#include <string.h>

#include "../../src/linux_abi/image.h"

typedef struct image_fake {
    hl_status open_status;
    hl_status metadata_status;
    hl_status read_status;
    hl_status close_status;
    uint32_t type;
    uint64_t reported_size;
    const uint8_t *input;
    size_t input_size;
    size_t read_limit;
    unsigned opens;
    unsigned reads;
    unsigned closes;
} image_fake;

static hl_host_result result(hl_status status, uint64_t value) {
    return (hl_host_result){status, 0, value, 0};
}

static hl_host_result image_open(void *context, hl_host_handle directory, const char *path, size_t path_size,
                                 uint32_t access, uint32_t creation, uint32_t permissions) {
    image_fake *fake = context;
    (void)creation;
    (void)permissions;
    if (directory != HL_HOST_HANDLE_CWD || path_size != 6 || memcmp(path, "/guest", 6) != 0 ||
        access != HL_HOST_FILE_READ)
        return result(HL_STATUS_INVALID_ARGUMENT, 0);
    fake->opens++;
    return result(fake->open_status, 7);
}

static hl_host_result image_metadata(void *context, hl_host_handle file, hl_host_file_metadata *output) {
    image_fake *fake = context;
    if (file != 7) return result(HL_STATUS_INVALID_ARGUMENT, 0);
    if (fake->metadata_status != HL_STATUS_OK) return result(fake->metadata_status, 0);
    memset(output, 0, sizeof(*output));
    output->type = fake->type;
    output->size = fake->reported_size;
    return result(HL_STATUS_OK, 0);
}

static hl_host_result image_read(void *context, hl_host_handle file, uint64_t offset, hl_host_bytes output) {
    image_fake *fake = context;
    size_t count;
    if (file != 7 || offset > fake->input_size) return result(HL_STATUS_INVALID_ARGUMENT, 0);
    fake->reads++;
    if (fake->read_status != HL_STATUS_OK) return result(fake->read_status, 0);
    count = fake->input_size - (size_t)offset;
    if (count > output.size) count = output.size;
    if (fake->read_limit != 0 && count > fake->read_limit) count = fake->read_limit;
    if (count != 0) memcpy(output.data, fake->input + offset, count);
    return result(HL_STATUS_OK, count);
}

static hl_host_result image_close(void *context, hl_host_handle file) {
    image_fake *fake = context;
    if (file != 7) return result(HL_STATUS_INVALID_ARGUMENT, 0);
    fake->closes++;
    return result(fake->close_status, 0);
}

static int run(image_fake *fake, hl_linux_image *image) {
    static const hl_host_file_services files = {
        .open_relative = image_open,
        .read_at = image_read,
        .metadata = image_metadata,
        .close = image_close,
    };
    hl_host_services services = {.context = fake, .file = &files};
    return hl_linux_image_read(&services, "/guest", image);
}

int main(void) {
    static const uint8_t input[] = {0x7f, 'E', 'L', 'F', 1, 2, 3};
    image_fake fake = {.open_status = HL_STATUS_OK,
                       .metadata_status = HL_STATUS_OK,
                       .read_status = HL_STATUS_OK,
                       .close_status = HL_STATUS_OK,
                       .type = HL_HOST_FILE_TYPE_REGULAR,
                       .reported_size = sizeof(input),
                       .input = input,
                       .input_size = sizeof(input),
                       .read_limit = 2};
    hl_linux_image image;

    HL_CHECK(run(&fake, &image) == 0);
    HL_CHECK(image.size == sizeof(input) && memcmp(image.bytes, input, sizeof(input)) == 0);
    HL_CHECK(fake.opens == 1 && fake.reads == 4 && fake.closes == 1);
    hl_linux_image_release(&image);

#define FAIL_FIELD(field, status)                                                                                      \
    do {                                                                                                               \
        image_fake failed = fake;                                                                                      \
        failed.opens = failed.reads = failed.closes = 0;                                                               \
        failed.field = status;                                                                                         \
        HL_CHECK(run(&failed, &image) != 0 && image.bytes == NULL && image.size == 0);                                 \
        HL_CHECK(failed.opens == 1);                                                                                   \
        HL_CHECK(failed.closes == (&failed.field == &failed.open_status ? 0u : 1u));                                   \
    } while (0)
    FAIL_FIELD(open_status, HL_STATUS_NOT_FOUND);
    FAIL_FIELD(metadata_status, HL_STATUS_IO);
    FAIL_FIELD(read_status, HL_STATUS_IO);
    FAIL_FIELD(close_status, HL_STATUS_IO);
#undef FAIL_FIELD

    fake.type = HL_HOST_FILE_TYPE_DIRECTORY;
    fake.opens = fake.reads = fake.closes = 0;
    HL_CHECK(run(&fake, &image) != 0 && fake.reads == 0 && fake.closes == 1);
    fake.type = HL_HOST_FILE_TYPE_REGULAR;
    fake.reported_size++;
    fake.opens = fake.reads = fake.closes = 0;
    HL_CHECK(run(&fake, &image) != 0 && fake.reads > 0 && fake.closes == 1);
    return 0;
}
