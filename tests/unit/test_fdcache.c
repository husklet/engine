#include "test.h"

#include "fdcache.h"
#include "hl/fake.h"

#include <string.h>

int main(void) {
    hl_fake_host fake;
    hl_host_services host;
    char root[] = "/root";
    size_t root_length = strlen(root);
    struct hl_linux_vfs_lower lowers[1] = {{"/lower", 6}};
    int lower_count = 1;
    int volume_count = 0;
    int threaded = 1;
    char fd_paths[4][HL_FDCACHE_PATH_CAPACITY] = {{0}};
    struct hl_linux_vfs_namespace vfs = {root, &root_length, lowers, &lower_count};
    hl_fdcache_binding binding = {
        &host, &vfs, &volume_count, root, &root_length, fd_paths, 4, &threaded, NULL,
    };
    struct stat stored = {0};
    struct stat found = {0};
    int result = 0;
    char path[64];

    hl_fake_host_init(&fake, &host);
    HL_CHECK(hl_fdcache_bind(&binding) == 0);

    stored.st_ino = 42;
    hl_fdcache_metadata_store("/root/file", 0, &stored);
    HL_CHECK(hl_fdcache_metadata_lookup("/root/file", &result, &found) == 1);
    HL_CHECK(result == 0 && found.st_ino == 42);
    hl_fdcache_resolution_bump();
    HL_CHECK(hl_fdcache_metadata_lookup("/root/file", &result, &found) == 1);

    hl_fdcache_metadata_store("/root/missing", -2, &stored);
    HL_CHECK(hl_fdcache_metadata_lookup("/root/missing", &result, &found) == 1 && result == -2);
    hl_fdcache_resolution_bump();
    HL_CHECK(hl_fdcache_metadata_lookup("/root/missing", &result, &found) == 0);

    hl_fdcache_resolution_store("/guest", "/root/guest");
    HL_CHECK(hl_fdcache_resolution_lookup("/guest", path, sizeof path) == 1);
    HL_CHECK(strcmp(path, "/root/guest") == 0);
    hl_fdcache_reset();
    HL_CHECK(hl_fdcache_resolution_lookup("/guest", path, sizeof path) == 0);

    HL_CHECK(hl_fdcache_dentry_cacheable(root) == 1);
    HL_CHECK(hl_fdcache_dentry_cacheable(lowers[0].canon) == 1);
    HL_CHECK(hl_fdcache_dentry_cacheable("/other") == 0);

    hl_fdcache_fd_setpath(2, "/root/file");
    HL_CHECK(strcmp(fd_paths[2], "/root/file") == 0);
    hl_fdcache_fd_clear(2);
    HL_CHECK(fd_paths[2][0] == 0);
    return EXIT_SUCCESS;
}
