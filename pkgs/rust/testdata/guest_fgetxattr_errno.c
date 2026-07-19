#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/xattr.h>
#include <unistd.h>

int main(void) {
    const char *path = "/tmp/fgetxattr-bound-errno";
    int descriptor = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (descriptor < 0) return 2;
    errno = 0;
    ssize_t by_path = getxattr(path, "system.posix_acl_access", NULL, 0);
    int path_error = errno;
    errno = 0;
    ssize_t by_descriptor = fgetxattr(descriptor, "system.posix_acl_access", NULL, 0);
    int descriptor_error = errno;
    printf("fgetxattr-missing path=%zd/%d descriptor=%zd/%d\n", by_path, path_error,
           by_descriptor, descriptor_error);
    close(descriptor);
    unlink(path);
    return by_path == -1 && path_error == ENODATA && by_descriptor == -1 &&
                   descriptor_error == ENODATA
               ? 0
               : 1;
}
