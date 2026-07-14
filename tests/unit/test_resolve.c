#define _POSIX_C_SOURCE 200809L

#include "../../src/host/resolve.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void make_file(int dirfd, const char *name, const char *value) {
    int fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    assert(write(fd, value, strlen(value)) == (ssize_t)strlen(value));
    assert(close(fd) == 0);
}

int main(void) {
    char temporary[] = "/tmp/hl-resolve-XXXXXX";
    char byte;
    hl_host_resolved_path resolved;
    int root;

    assert(mkdtemp(temporary) != NULL);
    root = open(temporary, O_RDONLY | O_DIRECTORY);
    assert(root >= 0);
    assert(mkdirat(root, "a", 0700) == 0);
    assert(mkdirat(root, "gone", 0700) == 0);
    assert(mkdirat(root, "deleted", 0700) == 0);
    make_file(root, "outside", "R");
    int a = openat(root, "a", O_RDONLY | O_DIRECTORY);
    assert(a >= 0);
    make_file(a, "file", "F");
    close(a);
    int gone = openat(root, "gone", O_RDONLY | O_DIRECTORY);
    assert(gone >= 0);
    make_file(gone, "marker", "M");
    close(gone);
    assert(symlinkat("a/file", root, "relative") == 0);
    assert(symlinkat("/outside", root, "absolute") == 0);
    assert(symlinkat("../../outside", root, "clamped") == 0);

    assert(hl_host_resolve_beneath(root, "a/../relative", 0, O_RDONLY, &resolved) == 0);
    assert(read(resolved.target_fd, &byte, 1) == 1 && byte == 'F');
    hl_host_resolved_path_destroy(&resolved);

    assert(hl_host_resolve_beneath(root, "relative", HL_HOST_RESOLVE_NOFOLLOW_FINAL, -1, &resolved) == 0);
    struct stat link_status;
    assert(fstatat(resolved.parent_fd, resolved.leaf, &link_status, AT_SYMLINK_NOFOLLOW) == 0);
    assert(S_ISLNK(link_status.st_mode));
    hl_host_resolved_path_destroy(&resolved);
    errno = 0;
    assert(hl_host_resolve_beneath(root, "relative", HL_HOST_RESOLVE_NO_SYMLINKS, -1, &resolved) == -1);
    assert(errno == ELOOP);

    assert(hl_host_resolve_beneath(root, "absolute", 0, O_RDONLY, &resolved) == 0);
    assert(read(resolved.target_fd, &byte, 1) == 1 && byte == 'R');
    hl_host_resolved_path_destroy(&resolved);

    assert(hl_host_resolve_beneath(root, "a/missing", 0, -1, &resolved) == 0);
    assert(strcmp(resolved.leaf, "missing") == 0);
    make_file(resolved.parent_fd, resolved.leaf, "N");
    hl_host_resolved_path_destroy(&resolved);

    assert(hl_host_resolve_beneath(root, "clamped", 0, O_RDONLY, &resolved) == 0);
    assert(read(resolved.target_fd, &byte, 1) == 1 && byte == 'R');
    hl_host_resolved_path_destroy(&resolved);

    assert(hl_host_resolve_beneath(root, "gone/marker", 0, -1, &resolved) == 0);
    assert(renameat(root, "gone", root, "renamed") == 0);
    assert(mkdirat(root, "gone", 0700) == 0);
    gone = openat(root, "gone", O_RDONLY | O_DIRECTORY);
    assert(gone >= 0);
    make_file(gone, "marker", "A");
    close(gone);
    int pinned_file = openat(resolved.parent_fd, resolved.leaf, O_RDONLY | O_NOFOLLOW);
    assert(pinned_file >= 0);
    assert(read(pinned_file, &byte, 1) == 1 && byte == 'M');
    close(pinned_file);
    make_file(resolved.parent_fd, "new", "G");
    hl_host_resolved_path_destroy(&resolved);

    assert(hl_host_resolve_beneath(root, "deleted/new", 0, -1, &resolved) == 0);
    assert(unlinkat(root, "deleted", AT_REMOVEDIR) == 0);
    struct stat pinned;
    assert(fstat(resolved.parent_fd, &pinned) == 0 && S_ISDIR(pinned.st_mode));
    hl_host_resolved_path_destroy(&resolved);

    close(root);
    return 0;
}
