#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

static sigjmp_buf jump;
static volatile sig_atomic_t armed;

static void fault(int signo) {
    (void)signo;
    if (armed) siglongjmp(jump, 1);
}

static int read_fault(volatile unsigned char *p) {
    armed = 1;
    if (sigsetjmp(jump, 1) == 0) {
        volatile unsigned char value = *p;
        (void)value;
        armed = 0;
        return 0;
    }
    armed = 0;
    return 1;
}

static int write_fault(volatile unsigned char *p) {
    armed = 1;
    if (sigsetjmp(jump, 1) == 0) {
        *p = 0x5a;
        armed = 0;
        return 0;
    }
    armed = 0;
    return 1;
}

int main(void) {
    struct sigaction sa = {0};
    const size_t page = 4096;
    int none_faults = 0, read_write_faults = 0, recovered = 1, toggles = 32;
    sa.sa_handler = fault;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    volatile unsigned char *a = mmap(NULL, page, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    volatile unsigned char *b = mmap(NULL, page, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a == MAP_FAILED || b == MAP_FAILED) return 2;
    a[0] = 0x11;
    a[page - 1] = 0x22;
    b[0] = 0x33;

    errno = 0;
    int unaligned = mprotect((void *)(a + 1), page, PROT_NONE) == -1 && errno == EINVAL;
    for (int i = 0; i < toggles; i++) {
        recovered &= mprotect((void *)a, 1, PROT_NONE) == 0;
        none_faults += read_fault(a);
        recovered &= mprotect((void *)a, page, PROT_READ) == 0;
        recovered &= a[0] == 0x11 && a[page - 1] == 0x22 && b[0] == 0x33;
        read_write_faults += write_fault(a);
        recovered &= mprotect((void *)a, page, PROT_READ | PROT_WRITE) == 0;
        a[0] = 0x11;
    }
    printf("mprotect enforce none=%d ro=%d recover=%d peer=%d unaligned=%d\n",
           none_faults == toggles, read_write_faults == toggles, recovered, b[0] == 0x33, unaligned);
    return 0;
}
