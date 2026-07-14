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
    hl_status close_status;
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

int main(void) {
    static const hl_host_file_services file = {
        HL_HOST_FILE_ABI, sizeof(file), file_open, NULL, NULL, NULL, NULL, file_close,
    };
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
    errno = 0;
    HL_CHECK(hl_host_file_create(NULL, "marker", 0644) == -1 && errno == EINVAL);
    errno = 0;
    HL_CHECK(hl_host_file_create(&services, "", 0644) == -1 && errno == EINVAL);
    return EXIT_SUCCESS;
}
