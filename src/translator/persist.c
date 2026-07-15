#include "persist.h"

#include <stdlib.h>
#include <string.h>

static int hl_persist_services(const hl_host_services *services) {
    return services != NULL && services->file != NULL && services->file->abi == HL_HOST_FILE_ABI &&
           services->file->open_relative != NULL && services->file->read_at != NULL &&
           services->file->metadata != NULL && services->file->close != NULL &&
           services->file->make_directory != NULL && services->file->validate_private_regular != NULL &&
           services->file->store_private_atomic != NULL && services->file->validate_private_directory != NULL;
}

static int hl_persist_parent(const hl_host_services *services, const char *path, hl_host_handle *parent,
                             const char **name, size_t *name_size) {
    const char *slash;
    char directory[1024];
    size_t directory_size;
    hl_host_result opened;
    if (path == NULL || parent == NULL || name == NULL || name_size == NULL) return 0;
    slash = strrchr(path, '/');
    if (slash == NULL || slash[1] == '\0') return 0;
    directory_size = slash == path ? 1 : (size_t)(slash - path);
    if (directory_size >= sizeof(directory)) return 0;
    memcpy(directory, path, directory_size);
    directory[directory_size] = '\0';
    opened = services->file->open_relative(services->context, HL_HOST_HANDLE_CWD, directory, directory_size,
                                           HL_HOST_FILE_READ | HL_HOST_FILE_DIRECTORY | HL_HOST_FILE_NOFOLLOW, 0, 0);
    if (opened.status != HL_STATUS_OK) return 0;
    if (services->file->validate_private_directory(services->context, opened.value).status != HL_STATUS_OK) {
        (void)services->file->close(services->context, opened.value);
        return 0;
    }
    *parent = opened.value;
    *name = slash + 1;
    *name_size = strlen(slash + 1);
    return 1;
}

int hl_persist_prepare(const hl_host_services *services, const char *directory) {
    hl_host_result result;
    if (!hl_persist_services(services) || directory == NULL || directory[0] == '\0') return 0;
    result = services->file->make_directory(services->context, HL_HOST_HANDLE_CWD, directory, strlen(directory), 0700);
    /* Existing directories are expected; opening it no-follow proves the object is a directory. */
    if (result.status != HL_STATUS_OK) {
        result = services->file->open_relative(services->context, HL_HOST_HANDLE_CWD, directory, strlen(directory),
                                               HL_HOST_FILE_READ | HL_HOST_FILE_DIRECTORY | HL_HOST_FILE_NOFOLLOW,
                                               0, 0);
        if (result.status != HL_STATUS_OK) return 0;
        if (services->file->validate_private_directory(services->context, result.value).status != HL_STATUS_OK) {
            (void)services->file->close(services->context, result.value);
            return 0;
        }
        return services->file->close(services->context, result.value).status == HL_STATUS_OK;
    }
    result = services->file->open_relative(services->context, HL_HOST_HANDLE_CWD, directory, strlen(directory),
                                           HL_HOST_FILE_READ | HL_HOST_FILE_DIRECTORY | HL_HOST_FILE_NOFOLLOW, 0, 0);
    if (result.status != HL_STATUS_OK) return 0;
    int ok = services->file->validate_private_directory(services->context, result.value).status == HL_STATUS_OK;
    if (services->file->close(services->context, result.value).status != HL_STATUS_OK) ok = 0;
    return ok;
}

int hl_persist_load(const hl_host_services *services, const char *path, uint64_t limit, void **data, size_t *size) {
    hl_host_result opened, result;
    hl_host_handle parent;
    const char *name;
    size_t name_size;
    hl_host_file_metadata metadata;
    unsigned char *bytes = NULL;
    uint64_t offset = 0;
    int ok = 0;
    if (data != NULL) *data = NULL;
    if (size != NULL) *size = 0;
    if (!hl_persist_services(services) || path == NULL || data == NULL || size == NULL) return 0;
    if (!hl_persist_parent(services, path, &parent, &name, &name_size)) return 0;
    opened = services->file->open_relative(services->context, parent, name, name_size,
                                           HL_HOST_FILE_READ | HL_HOST_FILE_NOFOLLOW, 0, 0);
    (void)services->file->close(services->context, parent);
    if (opened.status != HL_STATUS_OK) return 0;
    result = services->file->validate_private_regular(services->context, opened.value);
    if (result.status != HL_STATUS_OK) goto done;
    result = services->file->metadata(services->context, opened.value, &metadata);
    if (result.status != HL_STATUS_OK || metadata.type != HL_HOST_FILE_TYPE_REGULAR || metadata.size > limit ||
        metadata.size > SIZE_MAX)
        goto done;
    bytes = metadata.size != 0 ? malloc((size_t)metadata.size) : malloc(1);
    if (bytes == NULL) goto done;
    while (offset < metadata.size) {
        result = services->file->read_at(services->context, opened.value, offset,
                                         (hl_host_bytes){bytes + offset, (size_t)(metadata.size - offset)});
        if (result.status != HL_STATUS_OK || result.value == 0 || result.value > metadata.size - offset) goto done;
        offset += result.value;
    }
    ok = 1;
done:
    if (services->file->close(services->context, opened.value).status != HL_STATUS_OK) ok = 0;
    if (!ok) { free(bytes); return 0; }
    *data = bytes;
    *size = (size_t)metadata.size;
    return 1;
}

int hl_persist_store(const hl_host_services *services, const char *path, const void *data, size_t size) {
    hl_host_handle parent;
    const char *name;
    size_t name_size;
    if (!hl_persist_services(services) || path == NULL || (size != 0 && data == NULL)) return 0;
    if (!hl_persist_parent(services, path, &parent, &name, &name_size)) return 0;
    hl_host_result result = services->file->store_private_atomic(
        services->context, parent, name, name_size, (hl_host_const_bytes){data, size}, 0600);
    if (services->file->close(services->context, parent).status != HL_STATUS_OK) return 0;
    return result.status == HL_STATUS_OK;
}

void hl_persist_remove(const hl_host_services *services, const char *path) {
    hl_host_handle parent;
    const char *name;
    size_t name_size;
    if (hl_persist_services(services) && services->file->unlink_relative != NULL &&
        hl_persist_parent(services, path, &parent, &name, &name_size)) {
        (void)services->file->unlink_relative(services->context, parent, name, name_size);
        (void)services->file->close(services->context, parent);
    }
}

int hl_persist_take(hl_persist_cursor *cursor, void *output, size_t size) {
    if (cursor == NULL || (size != 0 && output == NULL) || cursor->offset > cursor->size ||
        size > cursor->size - cursor->offset)
        return 0;
    if (size != 0) memcpy(output, cursor->data + cursor->offset, size);
    cursor->offset += size;
    return 1;
}

int hl_persist_metadata(const hl_host_services *services, const char *path, hl_host_file_metadata *metadata) {
    hl_host_result opened, result;
    if (!hl_persist_services(services) || path == NULL || metadata == NULL) return 0;
    opened = services->file->open_relative(services->context, HL_HOST_HANDLE_CWD, path, strlen(path),
                                           HL_HOST_FILE_READ | HL_HOST_FILE_NOFOLLOW, 0, 0);
    if (opened.status != HL_STATUS_OK) return 0;
    result = services->file->metadata(services->context, opened.value, metadata);
    if (services->file->close(services->context, opened.value).status != HL_STATUS_OK) return 0;
    return result.status == HL_STATUS_OK;
}
