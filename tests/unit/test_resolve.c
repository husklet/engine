#define _POSIX_C_SOURCE 200809L

#include "../../src/host/resolve.h"

#include <assert.h>
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

    assert(hl_host_resolve_beneath(root, "a/../relative", O_RDONLY, &resolved) == 0);
    assert(read(resolved.target_fd, &byte, 1) == 1 && byte == 'F');
    hl_host_resolved_path_destroy(&resolved);

    assert(hl_host_resolve_beneath(root, "absolute", O_RDONLY, &resolved) == 0);
    assert(read(resolved.target_fd, &byte, 1) == 1 && byte == 'R');
    hl_host_resolved_path_destroy(&resolved);

    assert(hl_host_resolve_beneath(root, "a/missing", -1, &resolved) == 0);
    assert(strcmp(resolved.leaf, "missing") == 0);
    make_file(resolved.parent_fd, resolved.leaf, "N");
    hl_host_resolved_path_destroy(&resolved);

    assert(hl_host_resolve_beneath(root, "clamped", O_RDONLY, &resolved) == 0);
    assert(read(resolved.target_fd, &byte, 1) == 1 && byte == 'R');
    hl_host_resolved_path_destroy(&resolved);

    assert(hl_host_resolve_beneath(root, "gone/marker", -1, &resolved) == 0);
    assert(unlinkat(resolved.parent_fd, resolved.leaf, 0) == 0);
    assert(renameat(root, "gone", root, "renamed") == 0);
    make_file(resolved.parent_fd, "new", "G");
    hl_host_resolved_path_destroy(&resolved);

    assert(hl_host_resolve_beneath(root, "deleted/new", -1, &resolved) == 0);
    assert(unlinkat(root, "deleted", AT_REMOVEDIR) == 0);
    struct stat pinned;
    assert(fstat(resolved.parent_fd, &pinned) == 0 && S_ISDIR(pinned.st_mode));
    hl_host_resolved_path_destroy(&resolved);

    close(root);
    return 0;
}
