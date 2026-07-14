#define _GNU_SOURCE
#include "test.h"

#include "../../src/host/directory.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    char path[] = "/tmp/hl-directory-XXXXXX";
    HL_CHECK(mkdtemp(path) != NULL);
    int descriptor = open(path, O_RDONLY | O_DIRECTORY);
    HL_CHECK(descriptor >= 0);
    hl_host_directory directory = {0};
    HL_CHECK(hl_host_directory_init(&directory) == 0);
    HL_CHECK(hl_host_directory_set(&directory, descriptor, UINT64_C(91), HL_HOST_DIRECTORY_CREATE) == 0);
    int private_descriptor = hl_host_directory_descriptor(&directory);
    HL_CHECK(private_descriptor >= 0);
    HL_CHECK(hl_host_directory_relocate(&directory, private_descriptor) == 0);
    HL_CHECK(hl_host_directory_descriptor(&directory) >= 0 &&
             hl_host_directory_descriptor(&directory) != private_descriptor);
    pid_t child = fork();
    HL_CHECK(child >= 0);
    if (child == 0) {
        char file[128];
        int length = snprintf(file, sizeof(file), "%s/created", path);
        if (length <= 0 || (size_t)length >= sizeof(file)) _exit(2);
        int created = open(file, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (created < 0) _exit(3);
        close(created);
        _exit(0);
    }
    alarm(5);
    uint64_t token = 0;
    HL_CHECK(hl_host_directory_wait(&directory, &token) == 1);
    alarm(0);
    HL_CHECK(token == UINT64_C(91));
    HL_CHECK(hl_host_directory_remove(&directory, token) == 0);
    int status;
    HL_CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    hl_host_directory_close(&directory);
    char file[128];
    int length = snprintf(file, sizeof(file), "%s/created", path);
    HL_CHECK(length > 0 && (size_t)length < sizeof(file));
    HL_CHECK(unlink(file) == 0);
    HL_CHECK(rmdir(path) == 0);
    close(descriptor);
    return 0;
}
