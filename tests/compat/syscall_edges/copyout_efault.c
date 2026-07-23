// syscall-compat regression: a NULL copy-out buffer is -EFAULT, not a silent success.
//
// getrusage(2) and sched_getaffinity(2) unconditionally copy_to_user() their result struct, so a NULL
// destination faults. The engine guarded both copies with `if (pointer)` and returned SUCCESS when the
// pointer was NULL -- getrusage(RUSAGE_SELF, NULL) returned 0 and sched_getaffinity(0, size, NULL)
// returned the mask width, both without writing anything.
//
// Also pins fcntl(F_GETFL): a 64-bit Linux kernel forces O_LARGEFILE into f_flags for every open, so
// F_GETFL reports it on every fd. The engine rebuilt the flag word from scratch and dropped the bit.
// Arch-neutral: errnos and booleans only (O_LARGEFILE's VALUE differs per ISA, so test the bit).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

// Kernel O_LARGEFILE: 0100000 on x86-64, 0400000 on aarch64/asm-generic. glibc defines it as 0 under
// _FILE_OFFSET_BITS=64, so spell it out.
#if defined(__x86_64__)
#define K_O_LARGEFILE 0100000
#else
#define K_O_LARGEFILE 0400000
#endif

static int ec(long r) {
    return r == -1 ? errno : 0;
}

int main(void) {
    // getrusage: `who` is validated first (EINVAL), then the buffer (EFAULT) -- a NULL buffer with a
    // legal `who` must be EFAULT.
    printf("getrusage_null_errno=%d\n", ec(syscall(SYS_getrusage, 0 /*RUSAGE_SELF*/, (void *)0)));
    printf("getrusage_thread_null_errno=%d\n", ec(syscall(SYS_getrusage, 1 /*RUSAGE_THREAD*/, (void *)0)));
    printf("getrusage_badwho_errno=%d\n", ec(syscall(SYS_getrusage, -2, (void *)0)));

    // sched_getaffinity: cpusetsize is validated first (must be a multiple of sizeof(long) and wide enough
    // for every online CPU), then the mask is copied out -- a NULL mask at a legal size is EFAULT.
    printf("sched_getaffinity_null_errno=%d\n",
           ec(syscall(SYS_sched_getaffinity, 0, sizeof(cpu_set_t), (void *)0)));
    printf("sched_getaffinity_badsize_errno=%d\n", ec(syscall(SYS_sched_getaffinity, 0, 7, (void *)0)));

    // Control: a real buffer still succeeds for both.
    struct rusage ru;
    cpu_set_t mask;
    printf("getrusage_ok=%d\n", syscall(SYS_getrusage, 0, &ru) == 0);
    printf("sched_getaffinity_ok=%d\n", syscall(SYS_sched_getaffinity, 0, sizeof mask, &mask) > 0);

    // F_GETFL always carries O_LARGEFILE on 64-bit Linux, across the access modes and across a
    // F_SETFL(O_APPEND) round-trip.
    int fd = open("/dev/null", O_RDWR);
    int fl = fcntl(fd, F_GETFL);
    printf("getfl_largefile=%d getfl_accmode=%d\n", (fl & K_O_LARGEFILE) != 0, fl & O_ACCMODE);
    printf("setfl_append=%d\n", ec(fcntl(fd, F_SETFL, O_APPEND)));
    fl = fcntl(fd, F_GETFL);
    printf("getfl2_largefile=%d getfl2_append=%d getfl2_accmode=%d\n", (fl & K_O_LARGEFILE) != 0,
           (fl & O_APPEND) != 0, fl & O_ACCMODE);
    int rfd = open("/dev/null", O_RDONLY);
    printf("getfl_rdonly_largefile=%d\n", (fcntl(rfd, F_GETFL) & K_O_LARGEFILE) != 0);
    close(rfd);
    close(fd);
    return 0;
}
