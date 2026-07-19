#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static int publish(void *address, size_t length) {
#if defined(__aarch64__)
    uintptr_t cursor = (uintptr_t)address;
    uintptr_t end = cursor + length;
    for (; cursor < end; cursor += 64)
        __asm__ volatile("dc cvau, %0\n\tdsb ish\n\tic ivau, %0\n\tdsb ish\n\tisb" : : "r"(cursor) : "memory");
    return 0;
#else
    return msync(address, length, MS_SYNC);
#endif
}

int main(void) {
    const size_t page = 4096;
    int fd = (int)syscall(SYS_memfd_create, "offset-alias", 0u);
    if (fd < 0 || ftruncate(fd, (off_t)(page * 3)) != 0) return 2;

    unsigned char *readable = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, (off_t)page);
    unsigned char *writable = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)page);
    if (readable == MAP_FAILED || writable == MAP_FAILED) return 3;

    writable[0] = 0x0b;
    writable[page - 1] = 0x7d;
    int alias_coherent = readable[0] == 0x0b && readable[page - 1] == 0x7d;

    unsigned char *fixed = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fixed == MAP_FAILED ||
        mmap(fixed, page, PROT_NONE, MAP_SHARED | MAP_FIXED, fd, (off_t)page) != fixed)
        return 4;
    if (close(fd) != 0) return 5;
    int protected = mprotect(fixed, page, PROT_READ | PROT_EXEC) == 0;
    writable[1] = 0x36;
    writable[page - 2] = 0x9a;
    int fixed_coherent = protected && publish(fixed, page) == 0 && fixed[1] == 0x36 && fixed[page - 2] == 0x9a;

    writable[2] = 0x47;
    writable[page - 3] = 0xa5;
    int writable_publish_coherent = publish(writable, page) == 0 && fixed[2] == 0x47 && fixed[page - 3] == 0xa5;

    int unmapped = munmap(readable, page) == 0 && munmap(writable, page) == 0 && munmap(fixed, page) == 0;
    printf("memfd-offset-alias alias=%d fixed=%d writable-publish=%d unmapped=%d\n", alias_coherent, fixed_coherent,
           writable_publish_coherent, unmapped);
    return alias_coherent && fixed_coherent && writable_publish_coherent && unmapped ? 0 : 1;
}
