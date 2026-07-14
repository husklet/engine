#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum { BOUND_FD = 100 };

static int fail(int code) {
    return code;
}

int main(void) {
    struct stat status;
    char bytes[8] = {0};
    int duplicate;
    int fixed;
    int floored;
    pid_t child;
    int child_status;
    if (fstat(BOUND_FD, &status) != 0 || status.st_size != 4 || !S_ISREG(status.st_mode)) return fail(10);
    if (lseek(BOUND_FD, 0, SEEK_SET) != 0) return fail(11);
    if (read(BOUND_FD, bytes, 2) != 2 || memcmp(bytes, "ab", 2) != 0) return fail(12);
    duplicate = dup(BOUND_FD);
    if (duplicate < 0 || duplicate == BOUND_FD) return fail(13);
    if (read(duplicate, bytes, 1) != 1 || bytes[0] != 'c') return fail(14);
    fixed = dup3(BOUND_FD, 50, O_CLOEXEC);
    if (fixed != 50 || (fcntl(fixed, F_GETFD) & FD_CLOEXEC) == 0 || close(fixed) != 0) return fail(26);
    floored = fcntl(BOUND_FD, F_DUPFD_CLOEXEC, 40);
    if (floored < 40 || (fcntl(floored, F_GETFD) & FD_CLOEXEC) == 0 || close(floored) != 0) return fail(27);
    if ((fcntl(duplicate, F_GETFL) & O_ACCMODE) != O_RDWR || fcntl(duplicate, F_SETFL, 0) != 0 ||
        fcntl(duplicate, F_SETFD, FD_CLOEXEC) != 0 || (fcntl(duplicate, F_GETFD) & FD_CLOEXEC) == 0 ||
        fcntl(duplicate, F_SETFD, 0) != 0)
        return fail(28);
    if (pwrite(BOUND_FD, "X", 1, 0) != 1 || pread(duplicate, bytes, 1, 0) != 1 || bytes[0] != 'X') return fail(15);
    if (write(duplicate, "P", 1) != 1) return fail(16);
    child = fork();
    if (child < 0) return fail(17);
    if (child == 0) _exit(write(BOUND_FD, "C", 1) == 1 ? 0 : 18);
    if (waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
        return fail(19);
    if (write(duplicate, "D", 1) != 1) return fail(20);
    if (close(BOUND_FD) != 0) return fail(21);
    errno = 0;
    if (read(BOUND_FD, bytes, 1) != -1 || errno != EBADF) return fail(22);
    if (lseek(duplicate, 0, SEEK_SET) != 0 || read(duplicate, bytes, 6) != 6 || memcmp(bytes, "XbcPCD", 6) != 0)
        return fail(23);
    if (fstat(duplicate, &status) != 0 || status.st_size != 6) return fail(24);
    return close(duplicate) == 0 ? 0 : fail(25);
}
