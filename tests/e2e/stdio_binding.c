#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    struct stat input_status;
    struct stat output_status;
    char input[5];
    char floor_bytes[5];
    if (pread(64, floor_bytes, sizeof(floor_bytes), 0) != (ssize_t)sizeof(floor_bytes) ||
        memcmp(floor_bytes, "floor", sizeof(floor_bytes)))
        return 9;
    if (fstat(STDIN_FILENO, &input_status) != 0 || input_status.st_size != 5) return 10;
    if (fstat(STDOUT_FILENO, &output_status) != 0 || output_status.st_size != 0) return 11;
    if (lseek(STDIN_FILENO, 0, SEEK_SET) != 0) return 12;
    if (read(STDIN_FILENO, input, sizeof(input)) != (ssize_t)sizeof(input) || memcmp(input, "input", sizeof(input)))
        return 13;
    if (write(STDOUT_FILENO, "output", 6) != 6) return 14;
    if (write(STDERR_FILENO, "error", 5) != 5) return 18;
    if (close(STDIN_FILENO) != 0 || close(STDOUT_FILENO) != 0 || close(STDERR_FILENO) != 0) return 15;
    if (close(64) != 0) return 17;
    errno = 0;
    return read(STDIN_FILENO, input, 1) == -1 && errno == EBADF ? 0 : 16;
}
