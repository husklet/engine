#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { DIRECTORY_FD = 10 };

int main(void) {
    struct stat status;
    char bytes[5] = {0};
    int input;
    int alias;
    int output;
    int absolute;
    input = openat(DIRECTORY_FD, "input", O_RDONLY | O_CLOEXEC);
    if (input < 0 || (fcntl(input, F_GETFD) & FD_CLOEXEC) == 0) return 10;
    if (fstat(input, &status) != 0 || status.st_size != 5 || !S_ISREG(status.st_mode)) return 11;
    if (read(input, bytes, 2) != 2 || memcmp(bytes, "he", 2)) return 12;
    alias = dup(input);
    if (alias < 0 || close(input) != 0) return 13;
    if (read(alias, bytes + 2, 3) != 3 || memcmp(bytes, "hello", 5) || close(alias) != 0) return 14;
    output = openat(DIRECTORY_FD, "output", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    if (output < 0 || (fcntl(output, F_GETFD) & FD_CLOEXEC) == 0) return 15;
    if (write(output, "made", 4) != 4 || fstat(output, &status) != 0 || status.st_size != 4 || close(output) != 0)
        return 16;
    errno = 0;
    if (openat(DIRECTORY_FD, "missing", O_RDONLY) != -1 || errno != ENOENT) return 17;
    errno = 0;
    if (openat(DIRECTORY_FD, "input", O_RDONLY | O_NOFOLLOW) != -1 || errno != EINVAL) return 18;
    errno = 0;
    if (openat(DIRECTORY_FD, "", O_RDONLY) != -1 || errno != ENOENT) return 19;
    absolute = openat(DIRECTORY_FD, "/dev/null", O_RDONLY);
    if (absolute < 0 || close(absolute) != 0) return 20;
    return close(DIRECTORY_FD) == 0 ? 0 : 21;
}
