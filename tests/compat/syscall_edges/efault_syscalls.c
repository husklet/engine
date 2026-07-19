// syscall-compat regression: kernel-side bad-pointer validation. Raw syscalls handed a (void*)-1 user buffer
// must fail with EFAULT from the kernel, never fault the calling task. Each probe runs in its own child so a
// mis-translation that faults is observed deterministically (reported as sig=N) rather than killing the
// harness. Arch-neutral: per-probe errno (or signal) printed. Output is unbuffered so a crash cannot hide it.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define BAD ((long)-1)

// Runs `syscall(nr,...)` in a child; returns the errno on failure, 0 on success, or -sig if it was killed.
static int probe(long nr, long a, long b, long c, long d, long e) {
    pid_t p = fork();
    if (p == 0) {
        long r = syscall(nr, a, b, c, d, e);
        _exit(r == -1 ? (errno & 0x7f) : 0);
    }
    int st = 0;
    waitpid(p, &st, 0);
    if (WIFSIGNALED(st)) return -WTERMSIG(st);
    return WEXITSTATUS(st);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int fd = open("/dev/zero", O_RDONLY);
    printf("getrusage=%d\n", probe(SYS_getrusage, 0, BAD, 0, 0, 0));
    printf("newfstatat=%d\n", probe(SYS_newfstatat, fd, (long)"", BAD, 0x1000 /*AT_EMPTY_PATH*/, 0));
    printf("gettimeofday=%d\n", probe(SYS_gettimeofday, BAD, 0, 0, 0, 0));
    printf("times=%d\n", probe(SYS_times, BAD, 0, 0, 0, 0));
    printf("read=%d\n", probe(SYS_read, fd, BAD, 16, 0, 0));
    printf("pipe2=%d\n", probe(SYS_pipe2, BAD, 0, 0, 0, 0));
    return 0;
}
