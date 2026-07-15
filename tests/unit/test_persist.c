#include "test.h"

#include "../../src/translator/persist.h"

#include <string.h>

typedef struct persist_fake {
    const unsigned char *input;
    size_t input_size;
    unsigned char published[64];
    size_t published_size;
    hl_status trust_file;
    hl_status trust_directory;
    hl_status store_status;
    hl_status close_status;
    int truncate_read;
    int opened_parent;
    int opened_file;
    int closed_parent;
    int closed_file;
} persist_fake;

static hl_host_result result(hl_status status, uint64_t value) {
    return (hl_host_result){status, 0, value, 0};
}

static hl_host_result fake_open(void *context, hl_host_handle directory, const char *path, size_t path_size,
                                uint32_t access, uint32_t creation, uint32_t permissions) {
    persist_fake *fake = context;
    (void)creation; (void)permissions;
    if (directory == HL_HOST_HANDLE_CWD && path_size == 4 && memcmp(path, "/tmp", 4) == 0 &&
        (access & HL_HOST_FILE_DIRECTORY) != 0) {
        fake->opened_parent++;
        return result(HL_STATUS_OK, 1);
    }
    if (directory == 1 && path_size == 5 && memcmp(path, "cache", 5) == 0) {
        fake->opened_file++;
        return result(HL_STATUS_OK, 2);
    }
    return result(HL_STATUS_NOT_FOUND, 0);
}

static hl_host_result fake_read_at(void *context, hl_host_handle file, uint64_t offset, hl_host_bytes output) {
    persist_fake *fake = context;
    if (file != 2 || offset > fake->input_size) return result(HL_STATUS_INVALID_ARGUMENT, 0);
    size_t count = fake->input_size - (size_t)offset;
    if (count > output.size) count = output.size;
    if (fake->truncate_read && offset != 0) count = 0;
    if (count != 0) memcpy(output.data, fake->input + offset, count);
    return result(HL_STATUS_OK, count);
}

static hl_host_result fake_metadata(void *context, hl_host_handle file, hl_host_file_metadata *output) {
    persist_fake *fake = context;
    if (file != 2) return result(HL_STATUS_INVALID_ARGUMENT, 0);
    memset(output, 0, sizeof(*output));
    output->type = HL_HOST_FILE_TYPE_REGULAR;
    output->size = fake->input_size + (fake->truncate_read ? 1 : 0);
    return result(HL_STATUS_OK, 0);
}

static hl_host_result fake_close(void *context, hl_host_handle file) {
    persist_fake *fake = context;
    if (file == 1) fake->closed_parent++;
    if (file == 2) fake->closed_file++;
    return file == 1 || file == 2 ? result(fake->close_status, 0) : result(HL_STATUS_INVALID_ARGUMENT, 0);
}

static hl_host_result fake_mkdir(void *context, hl_host_handle directory, const char *path, size_t path_size,
                                 uint32_t permissions) {
    (void)context; (void)directory; (void)path; (void)path_size; (void)permissions;
    return result(HL_STATUS_ALREADY_EXISTS, 0);
}

static hl_host_result fake_private_file(void *context, hl_host_handle file) {
    persist_fake *fake = context;
    return file == 2 ? result(fake->trust_file, 0) : result(HL_STATUS_INVALID_ARGUMENT, 0);
}

static hl_host_result fake_private_directory(void *context, hl_host_handle directory) {
    persist_fake *fake = context;
    return directory == 1 ? result(fake->trust_directory, 0) : result(HL_STATUS_INVALID_ARGUMENT, 0);
}

static hl_host_result fake_store(void *context, hl_host_handle directory, const char *path, size_t path_size,
                                 hl_host_const_bytes input, uint32_t permissions) {
    persist_fake *fake = context;
    if (directory != 1 || path_size != 5 || memcmp(path, "cache", 5) != 0 || permissions != 0600 ||
        input.size > sizeof(fake->published))
        return result(HL_STATUS_INVALID_ARGUMENT, 0);
    if (fake->store_status != HL_STATUS_OK) return result(fake->store_status, 0);
    memcpy(fake->published, input.data, input.size);
    fake->published_size = input.size;
    return result(HL_STATUS_OK, 0);
}

static hl_host_result fake_unlink(void *context, hl_host_handle directory, const char *path, size_t path_size) {
    (void)context; (void)directory; (void)path; (void)path_size;
    return result(HL_STATUS_OK, 0);
}

int main(void) {
    static const hl_host_file_services file_template = {
        .abi = HL_HOST_FILE_ABI, .size = sizeof(file_template), .open_relative = fake_open, .read_at = fake_read_at,
        .metadata = fake_metadata, .close = fake_close, .make_directory = fake_mkdir,
        .validate_private_regular = fake_private_file, .store_private_atomic = fake_store,
        .validate_private_directory = fake_private_directory, .unlink_relative = fake_unlink,
    };
    hl_host_file_services file = file_template;
    persist_fake fake = {.input = (const unsigned char *)"cached", .input_size = 6};
    hl_host_services services = {.abi = HL_HOST_SERVICES_ABI, .size = sizeof(services), .context = &fake, .file = &file};
    void *data = NULL;
    size_t size = 0;
    hl_persist_directory directory;

    HL_CHECK(hl_persist_directory_open(&directory, &services, "/tmp", 1) == 1);
    HL_CHECK(hl_persist_load_at(&directory, "cache", 64, &data, &size) == 1 && size == 6);
    HL_CHECK(memcmp(data, "cached", 6) == 0);
    free(data);

    fake.trust_file = HL_STATUS_PERMISSION_DENIED;
    HL_CHECK(hl_persist_load_at(&directory, "cache", 64, &data, &size) == 0 && data == NULL && size == 0);
    fake.trust_file = HL_STATUS_OK;
    fake.truncate_read = 1;
    HL_CHECK(hl_persist_load_at(&directory, "cache", 64, &data, &size) == 0 && data == NULL);
    fake.truncate_read = 0;
    HL_CHECK(hl_persist_load_at(&directory, "cache", 5, &data, &size) == 0);

    HL_CHECK(hl_persist_store_at(&directory, "cache", "new", 3) == 1);
    HL_CHECK(fake.published_size == 3 && memcmp(fake.published, "new", 3) == 0);
    HL_CHECK(fake.opened_parent == 1);
    file.store_private_atomic = NULL;
    HL_CHECK(hl_persist_load_at(&directory, "cache", 64, &data, &size) == 1 && size == 6);
    free(data);
    file.store_private_atomic = fake_store;
    file.read_at = NULL;
    HL_CHECK(hl_persist_store_at(&directory, "cache", "new", 3) == 1);
    file.read_at = fake_read_at;
    fake.store_status = HL_STATUS_PLATFORM_FAILURE;
    HL_CHECK(hl_persist_store_at(&directory, "cache", "bad", 3) == 0);
    HL_CHECK(fake.published_size == 3 && memcmp(fake.published, "new", 3) == 0);

    HL_CHECK(hl_persist_store_at(&directory, "../cache", "bad", 3) == 0);
    HL_CHECK(hl_persist_store_at(&directory, ".", "bad", 3) == 0);
    HL_CHECK(hl_persist_store_at(&directory, "..", "bad", 3) == 0);
    HL_CHECK(fake.published_size == 3 && memcmp(fake.published, "new", 3) == 0);
    fake.close_status = HL_STATUS_PLATFORM_FAILURE;
    HL_CHECK(hl_persist_load_at(&directory, "cache", 64, &data, &size) == 0);
    fake.close_status = HL_STATUS_OK;
    HL_CHECK(hl_persist_remove_at(&directory, "cache") == 1);
    HL_CHECK(hl_persist_directory_close(&directory) == 1);
    HL_CHECK(fake.opened_parent == 1 && fake.closed_parent == 1);

    fake.trust_directory = HL_STATUS_PERMISSION_DENIED;
    HL_CHECK(hl_persist_directory_open(&directory, &services, "/tmp", 0) == 0);
    HL_CHECK(directory.handle == HL_HOST_HANDLE_INVALID);
    HL_CHECK(fake.opened_parent == 2 && fake.closed_parent == 2);

    const unsigned char cursor_data[] = {1, 2, 3};
    hl_persist_cursor cursor = {cursor_data, sizeof cursor_data, 0};
    unsigned char out[2];
    HL_CHECK(hl_persist_take(&cursor, out, sizeof out) == 1 && out[0] == 1 && out[1] == 2);
    HL_CHECK(hl_persist_take(&cursor, out, sizeof out) == 0 && cursor.offset == 2);
    return EXIT_SUCCESS;
}
