#define _GNU_SOURCE
#include <errno.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/sysinfo.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

int main(void) {
    unsigned char *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return 2;
    page[0] = 0x5a;
    if (mprotect(page, 4096, PROT_READ) != 0) return 3;

    int descriptors[2];
    if (pipe(descriptors) != 0 || write(descriptors[1], "x", 1) != 1) return 4;
    errno = 0;
    ssize_t read_result = read(descriptors[0], page, 1);
    int read_ok = read_result == -1 && errno == EFAULT && page[0] == 0x5a;

    errno = 0;
    long clock_result = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, page);
    int clock_ok = clock_result == -1 && errno == EFAULT && page[0] == 0x5a;

    errno = 0;
    int time_ok = syscall(SYS_clock_getres, CLOCK_MONOTONIC, page) == -1 && errno == EFAULT &&
                  syscall(SYS_gettimeofday, page, NULL) == -1 && errno == EFAULT &&
                  syscall(SYS_times, page) == -1 && errno == EFAULT && page[0] == 0x5a;

    uint64_t mask = 0;
    errno = 0;
    int signal_ok = syscall(SYS_rt_sigpending, page, 8) == -1 && errno == EFAULT &&
                    syscall(SYS_rt_sigprocmask, SIG_BLOCK, &mask, page, 8) == -1 && errno == EFAULT &&
                    syscall(SYS_rt_sigaction, SIGUSR1, NULL, page, 8) == -1 && errno == EFAULT &&
                    syscall(SYS_sigaltstack, NULL, page) == -1 && errno == EFAULT && page[0] == 0x5a;

    errno = 0;
    int misc_ok = syscall(SYS_uname, page) == -1 && errno == EFAULT &&
                  syscall(SYS_sysinfo, page) == -1 && errno == EFAULT &&
                  syscall(SYS_getrandom, page, 1, 0) == -1 && errno == EFAULT && page[0] == 0x5a;

    errno = 0;
    int seccomp_ok = syscall(SYS_seccomp, SECCOMP_GET_NOTIF_SIZES, 0, page) == -1 &&
                     errno == EFAULT && page[0] == 0x5a;

    void *resident = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int process_memory_ok = resident != MAP_FAILED &&
                            syscall(SYS_mincore, resident, 4096, page) == -1 && errno == EFAULT &&
                            prctl(PR_GET_PDEATHSIG, page) == -1 && errno == EFAULT &&
                            prctl(PR_GET_CHILD_SUBREAPER, page) == -1 && errno == EFAULT && page[0] == 0x5a;

    void *guard = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (guard == MAP_FAILED) return 5;
    errno = 0;
    int guard_ok = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, guard) == -1 && errno == EFAULT &&
                   syscall(SYS_nanosleep, guard, NULL) == -1 && errno == EFAULT &&
                   syscall(SYS_rt_sigpending, guard, 8) == -1 && errno == EFAULT &&
                   syscall(SYS_rt_sigprocmask, SIG_BLOCK, guard, NULL, 8) == -1 && errno == EFAULT &&
                   syscall(SYS_rt_sigaction, SIGUSR1, guard, NULL, 8) == -1 && errno == EFAULT &&
                   syscall(SYS_uname, guard) == -1 && errno == EFAULT &&
                   syscall(SYS_sysinfo, guard) == -1 && errno == EFAULT &&
                   syscall(SYS_getrandom, guard, 1, 0) == -1 && errno == EFAULT &&
                   syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, guard) == -1 && errno == EFAULT;

    printf("copyout-readonly read=%d clock=%d time=%d signal=%d misc=%d seccomp=%d process-memory=%d guard=%d\n",
           read_ok, clock_ok, time_ok, signal_ok, misc_ok, seccomp_ok, process_memory_ok, guard_ok);
    return !(read_ok && clock_ok && time_ok && signal_ok && misc_ok && seccomp_ok && process_memory_ok && guard_ok);
}
