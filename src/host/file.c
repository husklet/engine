#include "file.h"

#include <errno.h>
#include <string.h>

static int hl_host_file_marker(const hl_host_services *services, const char *path, uint32_t permissions,
                               uint32_t creation) {
    hl_host_result opened;
    if (services == NULL || services->file == NULL || services->file->open_relative == NULL ||
        services->file->close == NULL || path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    opened = services->file->open_relative(services->context, HL_HOST_HANDLE_CWD, path, strlen(path),
                                           HL_HOST_FILE_WRITE, creation, permissions);
    if (opened.status != HL_STATUS_OK) {
        errno = EIO;
        return -1;
    }
    (void)services->file->close(services->context, opened.value);
    return 0;
}

int hl_host_file_create(const hl_host_services *services, const char *path, uint32_t permissions) {
    return hl_host_file_marker(services, path, permissions, HL_HOST_FILE_CREATE);
}

int hl_host_file_exclusive(const hl_host_services *services, const char *path, uint32_t permissions) {
    return hl_host_file_marker(services, path, permissions, HL_HOST_FILE_CREATE | HL_HOST_FILE_EXCLUSIVE);
}

int hl_host_file_reset(const hl_host_services *services, const char *path, uint32_t permissions) {
    return hl_host_file_marker(services, path, permissions, HL_HOST_FILE_CREATE | HL_HOST_FILE_TRUNCATE);
}
