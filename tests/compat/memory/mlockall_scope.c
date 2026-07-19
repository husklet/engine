// mlockall/munlockall plus range mlock error paths. MCL_CURRENT locks the resident set, munlockall
// releases, an unknown mlockall flag is EINVAL, and mlock over an unmapped hole is ENOMEM. Advisory
// success/failure only; no residency numbers are printed.
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

static const char *en(int e) {
    switch (e) {
    case 0: return "0";
    case EINVAL: return "EINVAL";
    case ENOMEM: return "ENOMEM";
    case EPERM: return "EPERM";
    default: return "OTHER";
    }
}

int main(void) {
    long ps = sysconf(_SC_PAGESIZE);
    unsigned char *m = mmap(NULL, ps * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { printf("map fail\n"); return 2; }
    m[0] = 1; m[ps] = 2;

    int lock_range = mlock(m, ps * 2) == 0;
    int unlock_range = munlock(m, ps * 2) == 0;

    errno = 0; int all = mlockall(MCL_CURRENT); const char *all_e = all == 0 ? "0" : en(errno);
    int unall = munlockall() == 0;
    errno = 0; int bad = mlockall(0x40); const char *bad_e = bad == 0 ? "0" : en(errno);

    // mlock spanning an unmapped hole (middle page removed) -> ENOMEM, none of the range gets locked.
    munmap(m + ps, ps);
    errno = 0; int hole = mlock(m, ps * 2); const char *hole_e = hole == 0 ? "0" : en(errno);

    munmap(m, ps);
    printf("lock_range=%d unlock_range=%d mlockall=%s munlockall=%d badflag=%s hole=%s\n", lock_range,
           unlock_range, all_e, unall, bad_e, hole_e);
    return 0;
}
