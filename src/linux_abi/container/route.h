#ifndef HL_LINUX_VFS_ROUTE_H
#define HL_LINUX_VFS_ROUTE_H

#include <stddef.h>

static int rc_lookup(const char *guest, char *host, size_t size);
static void rc_store(const char *guest, const char *host);

static const char *shm_backing_path(const char *guest, char *buffer, size_t size);
static const char *atpath(int directory_fd, const char *raw, char *buffer, size_t size, int nofollow);

#endif
