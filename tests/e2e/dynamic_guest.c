#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int constructor_value;
static int relocated_value = 11;
static int *relocated_pointer = &relocated_value;
static _Thread_local int tls_value = 23;

static void __attribute__((constructor)) initialize(void) {
    constructor_value = *relocated_pointer + 6;
}

int main(int argc, char **argv) {
    char file_data[32] = {0};
    char *output;
    const char *path = getenv("PATH");
    int fd = open("/etc/hl-dynamic-data", O_RDONLY);
    int result_fd;
    ssize_t count;
    int length;
    if (fd < 0) return 10;
    count = read(fd, file_data, sizeof file_data - 1);
    if (close(fd) != 0 || count < 0) return 11;
    file_data[count] = '\0';
    if (argc != 2 || strcmp(argv[1], "probe") != 0 || path == NULL || path[0] == '\0') return 12;
    output = malloc(160);
    if (output == NULL) return 13;
    length = snprintf(output, 160, "dynamic-ok ctor=%d tls=%d file=%s path=%d arg=%s\n", constructor_value,
                      tls_value, file_data, path[0] == '/', argv[1]);
    if (length <= 0 || length >= 160) {
        free(output);
        return 14;
    }
    result_fd = open("/tmp/hl-dynamic-result", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (result_fd < 0 || write(result_fd, output, (size_t)length) != length || close(result_fd) != 0) {
        if (result_fd >= 0) close(result_fd);
        free(output);
        return 15;
    }
    if (write(STDOUT_FILENO, output, (size_t)length) != length) {
        free(output);
        return 16;
    }
    free(output);
    return 0;
}
