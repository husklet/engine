#define _GNU_SOURCE
#include "test.h"

#include "../../src/linux_abi/container/owner.h"

#include <fcntl.h>
#include <sys/wait.h>

int main(void) {
    char root[] = "/tmp/hl-owner-test-XXXXXX";
    char file[256];
    char hard[256];
    struct stat status;
    int uid;
    int gid;
    int pipefd[2];

    HL_CHECK(mkdtemp(root) != NULL);
    snprintf(file, sizeof(file), "%s/file", root);
    snprintf(hard, sizeof(hard), "%s/hard", root);
    int descriptor = open(file, O_CREAT | O_RDWR | O_TRUNC, 0600);
    HL_CHECK(descriptor >= 0);
    close(descriptor);
    HL_CHECK(link(file, hard) == 0);
    HL_CHECK(hl_owner_seed(root, "file\t12\t34\n") == 0);
    HL_CHECK(lstat(file, &status) == 0);
    HL_CHECK(hl_owner_get(file, -1, &status, 1, &uid, &gid));
    HL_CHECK(uid == 12 && gid == 34);
    HL_CHECK(lstat(hard, &status) == 0);
    HL_CHECK(hl_owner_get(hard, -1, &status, 1, &uid, &gid));
    HL_CHECK(uid == 12 && gid == 34);

    descriptor = open(file, O_RDONLY);
    HL_CHECK(descriptor >= 0);
    hl_owner_set_fd(descriptor, 23, 45);
    HL_CHECK(close(descriptor) == 0);
    HL_CHECK(hl_owner_get(file, -1, &status, 1, &uid, &gid));
    HL_CHECK(uid == 23 && gid == 45);

    HL_CHECK(pipe(pipefd) == 0);
    pid_t child = fork();
    HL_CHECK(child >= 0);
    if (child == 0) {
        close(pipefd[0]);
        hl_owner_set_path(hard, 56, 78, 1);
        HL_CHECK(write(pipefd[1], "x", 1) == 1);
        _exit(0);
    }
    close(pipefd[1]);
    char byte;
    HL_CHECK(read(pipefd[0], &byte, 1) == 1);
    HL_CHECK(waitpid(child, NULL, 0) == child);
    HL_CHECK(lstat(file, &status) == 0);
    HL_CHECK(hl_owner_get(file, -1, &status, 1, &uid, &gid));
    HL_CHECK(uid == 56 && gid == 78);

    uint64_t birth = hl_owner_birth(file, -1, 1, &status);
    HL_CHECK(hl_owner_slot((uint64_t)status.st_dev, (uint64_t)status.st_ino, birth + 1, 0) == NULL);
    HL_CHECK(unlink(hard) == 0 && unlink(file) == 0 && rmdir(root) == 0);
    return EXIT_SUCCESS;
}
