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
    read = services.file->read(services.context, resolved.target, &value, 1);
    HL_CHECK(read.status == HL_STATUS_OK && read.value == 1 && value == 'x');
    HL_CHECK(services.file->close(services.context, resolved.target).status == HL_STATUS_OK);
    HL_CHECK(services.file->close(services.context, resolved.parent).status == HL_STATUS_OK);
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
    HL_CHECK(services.file->close(services.context, root.value).status == HL_STATUS_OK);
    test_destroy(host);
    unlink(fifo);
    unlink(file);
    rmdir(child);
    rmdir(temporary);
    return EXIT_SUCCESS;
}
