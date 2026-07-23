#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001u
#define MFD_ALLOW_SEALING 0x0002u
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#endif

static int prepare_output(const char *release) {
    char output[1024];
    if (snprintf(output, sizeof output, "%s.output", release) >= (int)sizeof output) return -1;
    int fd = open(output, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    if (snprintf(output, sizeof output, "%s.error", release) >= (int)sizeof output) return -1;
    fd = open(output, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0 || dup2(fd, STDERR_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    fd = open("/dev/null", O_RDONLY);
    if (fd < 0 || dup2(fd, STDIN_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    return 0;
}

int main(int argc, char **argv) {
    static const char initial[] = "0123456789abcdef";
    char byte = 0;
    if (argc != 2 || prepare_output(argv[1]) != 0) return 2;
    int fd = (int)syscall(SYS_memfd_create, "checkpoint-map", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, 4096) != 0 || pwrite(fd, initial, sizeof initial, 0) != (ssize_t)sizeof initial)
        return 3;
    unsigned char *mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED || memcmp(mapping, initial, sizeof initial) != 0) return 4;
    int duplicate = dup(fd);
    if (duplicate < 0 || lseek(fd, 3, SEEK_SET) != 3 ||
        fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK) != 0)
        return 5;
    dprintf(STDOUT_FILENO, "READY 1\n");
    while (access(argv[1], F_OK) != 0)
        if (errno != ENOENT) return 6;

    int seals = fcntl(fd, F_GET_SEALS);
    if (seals != (F_SEAL_GROW | F_SEAL_SHRINK)) {
        dprintf(STDERR_FILENO, "memfd seals=%d errno=%d\n", seals, errno);
        return 7;
    }
    if (read(duplicate, &byte, 1) != 1 || byte != '3' || lseek(fd, 0, SEEK_CUR) != 4) return 8;
    if (pwrite(fd, "X", 1, 5) != 1 || mapping[5] != 'X') return 9;
    mapping[6] = 'Y';
    if (pread(fd, &byte, 1, 6) != 1 || byte != 'Y') return 10;
    errno = 0;
    if (ftruncate(fd, 8192) == 0 || errno != EPERM) return 11;
    errno = 0;
    if (ftruncate(fd, 2048) == 0 || errno != EPERM) return 12;
    munmap(mapping, 4096);
    close(duplicate);
    close(fd);
    dprintf(STDOUT_FILENO, "MEMFD-RESTORED\n");
    return 0;
}
