#include "test.h"

#include "../../src/host/file.h"

#include <errno.h>
#include <string.h>

typedef struct file_test {
    char path[64];
    size_t path_size;
    hl_host_handle directory;
    uint32_t access;
    uint32_t creation;
    uint32_t permissions;
    hl_host_handle closed;
    hl_status open_status;
    hl_status write_status;
    hl_status close_status;
    unsigned char written[64];
    size_t written_size;
    size_t write_limit;
    char old_path[64];
    char new_path[64];
    char unlinked_path[64];
    hl_status namespace_status;
} file_test;

static hl_host_result file_open(void *context, hl_host_handle directory, const char *path, size_t path_size,
                                uint32_t access, uint32_t creation, uint32_t permissions) {
    file_test *test = context;
    test->directory = directory;
    test->path_size = path_size;
    memcpy(test->path, path, path_size);
    test->path[path_size] = '\0';
    test->access = access;
    test->creation = creation;
    test->permissions = permissions;
    return (hl_host_result){test->open_status, 0, 77, 0};
}

static hl_host_result file_close(void *context, hl_host_handle file) {
    file_test *test = context;
    test->closed = file;
    return (hl_host_result){test->close_status, 0, 0, 0};
}

static hl_host_result file_write(void *context, hl_host_handle file, const void *input, uint64_t input_size) {
    file_test *test = context;
    size_t count = (size_t)input_size;
    if (file != 77) return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    if (test->write_status != HL_STATUS_OK) return (hl_host_result){test->write_status, 0, 0, 0};
    if (test->write_limit != 0 && count > test->write_limit) count = test->write_limit;
    if (test->written_size + count > sizeof(test->written)) return (hl_host_result){HL_STATUS_RESOURCE_LIMIT, 0, 0, 0};
    memcpy(test->written + test->written_size, input, count);
    test->written_size += count;
    return (hl_host_result){HL_STATUS_OK, 0, count, 0};
}

static hl_host_result file_rename(void *context, hl_host_handle old_directory, const char *old_path,
                                  size_t old_path_size, hl_host_handle new_directory, const char *new_path,
                                  size_t new_path_size) {
    file_test *test = context;
    if (old_directory != HL_HOST_HANDLE_CWD || new_directory != HL_HOST_HANDLE_CWD ||
        old_path_size >= sizeof(test->old_path) || new_path_size >= sizeof(test->new_path))
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    memcpy(test->old_path, old_path, old_path_size);
    test->old_path[old_path_size] = '\0';
    memcpy(test->new_path, new_path, new_path_size);
    test->new_path[new_path_size] = '\0';
    return (hl_host_result){test->namespace_status, 0, 0, 0};
}

static hl_host_result file_unlink(void *context, hl_host_handle directory, const char *path, size_t path_size) {
    file_test *test = context;
    if (directory != HL_HOST_HANDLE_CWD || path_size >= sizeof(test->unlinked_path))
        return (hl_host_result){HL_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    memcpy(test->unlinked_path, path, path_size);
    test->unlinked_path[path_size] = '\0';
    return (hl_host_result){test->namespace_status, 0, 0, 0};
}

int main(void) {
    static const hl_host_file_services file = {.abi = HL_HOST_FILE_ABI,
                                               .size = sizeof(file),
                                               .open_relative = file_open,
                                               .close = file_close,
                                               .write = file_write,
                                               .rename_relative = file_rename,
                                               .unlink_relative = file_unlink};
    file_test test = {0};
    hl_host_services services = {
        .abi = HL_HOST_SERVICES_ABI,
        .size = sizeof(services),
        .capabilities = HL_HOST_CAP_FILE,
        .context = &test,
        .file = &file,
    };

    HL_CHECK(hl_host_file_create(&services, "/tmp/marker", 0644) == 0);
    HL_CHECK(test.directory == HL_HOST_HANDLE_CWD);
    HL_CHECK(strcmp(test.path, "/tmp/marker") == 0 && test.path_size == strlen("/tmp/marker"));
    HL_CHECK(test.access == HL_HOST_FILE_WRITE && test.creation == HL_HOST_FILE_CREATE);
    HL_CHECK(test.permissions == 0644 && test.closed == 77);

    memset(&test, 0, sizeof(test));
    HL_CHECK(hl_host_file_reset(&services, "done", 0600) == 0);
    HL_CHECK(test.creation == (HL_HOST_FILE_CREATE | HL_HOST_FILE_TRUNCATE));
    HL_CHECK(test.permissions == 0600 && test.closed == 77);

    memset(&test, 0, sizeof(test));
    HL_CHECK(hl_host_file_exclusive(&services, "child", 0644) == 0);
    HL_CHECK(test.creation == (HL_HOST_FILE_CREATE | HL_HOST_FILE_EXCLUSIVE));
    HL_CHECK(test.permissions == 0644 && test.closed == 77);

    memset(&test, 0, sizeof(test));
    test.close_status = HL_STATUS_PLATFORM_FAILURE;
    HL_CHECK(hl_host_file_create(&services, "marker", 0644) == 0 && test.closed == 77);

    memset(&test, 0, sizeof(test));
    test.open_status = HL_STATUS_PERMISSION_DENIED;
    errno = 0;
    HL_CHECK(hl_host_file_create(&services, "marker", 0644) == -1 && errno == EIO && test.closed == 0);

    memset(&test, 0, sizeof(test));
    test.write_limit = 2;
    HL_CHECK(hl_host_file_store(&services, "registry", 0640, "payload", 7) == 0);
    HL_CHECK(test.creation == (HL_HOST_FILE_CREATE | HL_HOST_FILE_TRUNCATE));
    HL_CHECK(test.permissions == 0640 && test.closed == 77);
    HL_CHECK(test.written_size == 7 && memcmp(test.written, "payload", 7) == 0);

    memset(&test, 0, sizeof(test));
    test.write_status = HL_STATUS_PLATFORM_FAILURE;
    errno = 0;
    HL_CHECK(hl_host_file_store(&services, "registry", 0644, "x", 1) == -1 && errno == EIO);
    HL_CHECK(test.closed == 77);

    memset(&test, 0, sizeof(test));
    test.close_status = HL_STATUS_PLATFORM_FAILURE;
    errno = 0;
    HL_CHECK(hl_host_file_store(&services, "registry", 0644, "", 0) == -1 && errno == EIO);

    errno = 0;
    HL_CHECK(hl_host_file_store(&services, "registry", 0644, NULL, 1) == -1 && errno == EINVAL);

    memset(&test, 0, sizeof(test));
    HL_CHECK(hl_host_file_rename(&services, "temporary", "published") == 0);
    HL_CHECK(strcmp(test.old_path, "temporary") == 0 && strcmp(test.new_path, "published") == 0);
    HL_CHECK(hl_host_file_unlink(&services, "published") == 0);
    HL_CHECK(strcmp(test.unlinked_path, "published") == 0);

    memset(&test, 0, sizeof(test));
    test.namespace_status = HL_STATUS_PERMISSION_DENIED;
    errno = 0;
    HL_CHECK(hl_host_file_rename(&services, "temporary", "published") == -1 && errno == EIO);
    errno = 0;
    HL_CHECK(hl_host_file_unlink(&services, "published") == -1 && errno == EIO);
    errno = 0;
    HL_CHECK(hl_host_file_create(NULL, "marker", 0644) == -1 && errno == EINVAL);
    errno = 0;
    HL_CHECK(hl_host_file_create(&services, "", 0644) == -1 && errno == EINVAL);
    return EXIT_SUCCESS;
}
