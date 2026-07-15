#ifndef HL_LINUX_VFS_ROUTE_H
#define HL_LINUX_VFS_ROUTE_H

#include <stddef.h>

static const char *shm_backing_path(const char *guest, char *buffer, size_t size);
static const char *atpath(int directory_fd, const char *raw, char *buffer, size_t size, int nofollow);

#endif
