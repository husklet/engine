#define _GNU_SOURCE

#include "test.h"

#ifdef HL_TEST_HOST_MACOS
#include "hl/macos.h"
#define test_host hl_host_macos
#define test_create hl_host_macos_create
#define test_destroy hl_host_macos_destroy
#else
#include "hl/linux.h"
#define test_host hl_host_linux
#define test_create hl_host_linux_create
#define test_destroy hl_host_linux_destroy
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char temporary[] = "/tmp/hl-resolve-services-XXXXXX";
    char child[512];
    char file[512];
    char fifo[512];
    char created[512];
    char escaped[512];
    char value = 0;
    test_host *host;
    hl_host_services services;
    hl_host_result root;
    hl_host_result read;
    hl_host_file_resolution resolved;
    int descriptor;
    HL_CHECK(mkdtemp(temporary) != NULL);
    snprintf(child, sizeof(child), "%s/a", temporary);
    snprintf(file, sizeof(file), "%s/a/file", temporary);
    snprintf(fifo, sizeof(fifo), "%s/a/fifo", temporary);
    snprintf(created, sizeof(created), "%s/a/created", temporary);
    snprintf(escaped, sizeof(escaped), "%s/escape", temporary);
    HL_CHECK(mkdir(child, 0700) == 0);
    descriptor = open(file, O_WRONLY | O_CREAT | O_EXCL, 0600);
    HL_CHECK(descriptor >= 0 && write(descriptor, "x", 1) == 1 && close(descriptor) == 0);
    HL_CHECK(mkfifo(fifo, 0600) == 0);
    HL_CHECK(test_create(&host, &services) == HL_STATUS_OK);
    root = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, temporary, strlen(temporary),
                                        HL_HOST_FILE_READ | HL_HOST_FILE_DIRECTORY, 0, 0);
    HL_CHECK(root.status == HL_STATUS_OK);
    HL_CHECK(services.file->resolve_beneath(services.context, root.value, "a/file", 6, 0, &resolved).status ==
             HL_STATUS_OK);
    HL_CHECK(resolved.target_type == HL_HOST_FILE_TYPE_REGULAR && strcmp(resolved.final, "file") == 0);
    HL_CHECK(services.file->close(services.context, resolved.target).status == HL_STATUS_OK);
    HL_CHECK(services.file->close(services.context, resolved.parent).status == HL_STATUS_OK);
    read = services.file->open_beneath(services.context, root.value, "a/file", 6, HL_HOST_FILE_READ, 0, 0, 0);
    HL_CHECK(read.status == HL_STATUS_OK);
    HL_CHECK(services.file->read(services.context, read.value, &value, 1).status == HL_STATUS_OK && value == 'x');
    HL_CHECK(services.file->close(services.context, read.value).status == HL_STATUS_OK);
    /* open_beneath resolves an existing target before reopening it from the
       pinned parent. Both temporary handles must be released on every call;
       this count exceeds the handle capacity of both host implementations. */
    for (size_t iteration = 0; iteration < 5000; ++iteration) {
        read = services.file->open_beneath(services.context, root.value, "a/file", 6, HL_HOST_FILE_READ, 0, 0, 0);
        HL_CHECK(read.status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, read.value).status == HL_STATUS_OK);
    }
    /* PATH_ONLY is the portable Linux O_PATH contract: creation and
       truncation flags are ignored on every host. */
    read = services.file->open_beneath(services.context, root.value, "a/path-missing", 14, HL_HOST_FILE_PATH_ONLY,
                                       HL_HOST_FILE_CREATE | HL_HOST_FILE_EXCLUSIVE, 0600, 0);
    HL_CHECK(read.status == HL_STATUS_NOT_FOUND);
    {
        char path_missing[512];
        struct stat status;
        snprintf(path_missing, sizeof(path_missing), "%s/a/path-missing", temporary);
        HL_CHECK(lstat(path_missing, &status) != 0 && errno == ENOENT);
    }
    read = services.file->open_beneath(services.context, root.value, "a/file", 6, HL_HOST_FILE_PATH_ONLY,
                                       HL_HOST_FILE_TRUNCATE, 0, 0);
    HL_CHECK(read.status == HL_STATUS_OK);
    HL_CHECK(services.file->close(services.context, read.value).status == HL_STATUS_OK);
    {
        struct stat status;
        HL_CHECK(stat(file, &status) == 0 && status.st_size == 1);
    }
    /* Resolving a FIFO is a metadata operation and must neither wait for a peer nor consume data. */
    alarm(2);
    HL_CHECK(services.file->resolve_beneath(services.context, root.value, "a/fifo", 6, 0, &resolved).status ==
             HL_STATUS_OK);
    alarm(0);
    HL_CHECK(resolved.target_type == HL_HOST_FILE_TYPE_FIFO && strcmp(resolved.final, "fifo") == 0);
    HL_CHECK(services.file->close(services.context, resolved.target).status == HL_STATUS_OK);
    HL_CHECK(services.file->close(services.context, resolved.parent).status == HL_STATUS_OK);
    HL_CHECK(
        services.file
            ->resolve_beneath(services.context, root.value, "a/missing", 9, HL_HOST_RESOLVE_ALLOW_MISSING, &resolved)
            .status == HL_STATUS_OK);
    HL_CHECK(resolved.target == HL_HOST_HANDLE_INVALID && strcmp(resolved.final, "missing") == 0);
    HL_CHECK(services.file->close(services.context, resolved.parent).status == HL_STATUS_OK);
    /* Creation and opening happen through the pinned parent, with one no-follow final open. */
    read = services.file->open_beneath(services.context, root.value, "a/created", 9,
                                       HL_HOST_FILE_READ | HL_HOST_FILE_WRITE,
                                       HL_HOST_FILE_CREATE | HL_HOST_FILE_EXCLUSIVE, 0600, 0);
    HL_CHECK(read.status == HL_STATUS_OK);
    HL_CHECK(services.file->write(services.context, read.value, "new", 3).value == 3);
    HL_CHECK(services.file->close(services.context, read.value).status == HL_STATUS_OK);
    descriptor = open(created, O_RDONLY);
    {
        char bytes[3];
        HL_CHECK(descriptor >= 0 && pread(descriptor, bytes, sizeof(bytes), 0) == 3 && memcmp(bytes, "new", 3) == 0 &&
                 close(descriptor) == 0);
    }
    /* Absolute input and embedded NULs cannot change the root interpretation. */
    HL_CHECK(
        services.file->open_beneath(services.context, root.value, "/a/file", 7, HL_HOST_FILE_READ, 0, 0, 0).status ==
        HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(
        services.file->open_beneath(services.context, root.value, "a\0/file", 7, HL_HOST_FILE_READ, 0, 0, 0).status ==
        HL_STATUS_INVALID_ARGUMENT);
    /* Parent traversal clamps at root rather than escaping it. */
    descriptor = open(escaped, O_WRONLY | O_CREAT | O_EXCL, 0600);
    HL_CHECK(descriptor >= 0 && write(descriptor, "r", 1) == 1 && close(descriptor) == 0);
    read = services.file->open_beneath(services.context, root.value, "../../escape", 12, HL_HOST_FILE_READ, 0, 0, 0);
    HL_CHECK(read.status == HL_STATUS_OK);
    HL_CHECK(services.file->close(services.context, read.value).status == HL_STATUS_OK);
    /* A final link is rejected when requested; it cannot be swapped into an escape. */
    HL_CHECK(symlink("../../../../etc/passwd", fifo) != 0); /* fifo already occupies the name */
    HL_CHECK(unlink(fifo) == 0);
    HL_CHECK(symlink("file", fifo) == 0);
    read = services.file->open_beneath(services.context, root.value, "a/fifo", 6, HL_HOST_FILE_READ, 0, 0, 0);
    HL_CHECK(read.status == HL_STATUS_OK);
    value = 0;
    HL_CHECK(services.file->read(services.context, read.value, &value, 1).status == HL_STATUS_OK && value == 'x');
    HL_CHECK(services.file->close(services.context, read.value).status == HL_STATUS_OK);
    HL_CHECK(services.file
                 ->open_beneath(services.context, root.value, "a/fifo", 6, HL_HOST_FILE_READ, 0, 0,
                                HL_HOST_RESOLVE_NOFOLLOW_FINAL)
                 .status != HL_STATUS_OK);
    /* O_CREAT|O_EXCL observes the directory entry itself: both a link whose
       target exists and a dangling link are EEXIST, and neither is followed. */
    HL_CHECK(services.file
                 ->open_beneath(services.context, root.value, "a/fifo", 6, HL_HOST_FILE_READ | HL_HOST_FILE_WRITE,
                                HL_HOST_FILE_CREATE | HL_HOST_FILE_EXCLUSIVE, 0600, 0)
                 .status == HL_STATUS_ALREADY_EXISTS);
    HL_CHECK(unlink(fifo) == 0);
    HL_CHECK(symlink("missing-target", fifo) == 0);
    HL_CHECK(services.file
                 ->open_beneath(services.context, root.value, "a/fifo", 6, HL_HOST_FILE_READ | HL_HOST_FILE_WRITE,
                                HL_HOST_FILE_CREATE | HL_HOST_FILE_EXCLUSIVE, 0600, 0)
                 .status == HL_STATUS_ALREADY_EXISTS);
    HL_CHECK(services.file->close(services.context, root.value).status == HL_STATUS_OK);
    test_destroy(host);
    unlink(fifo);
    unlink(created);
    unlink(escaped);
    unlink(file);
    rmdir(child);
    rmdir(temporary);
    return EXIT_SUCCESS;
}
