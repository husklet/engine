#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#if !defined(__aarch64__)
int main(void) {
    return 0;
}
#else

__attribute__((naked, noinline, aligned(16))) static unsigned patchable(void) {
    __asm__ volatile("mov w0,#11\n\tret");
}

static unsigned call_patchable(void) {
    unsigned (*volatile function)(void) = patchable;
    return function();
}

int main(int argc, char **argv, char **environment) {
    unsigned before = 0;
    for (unsigned i = 0; i < 256; i++) before = call_patchable();

    if (argc == 1) {
        /*
         * execve makes the engine save this translated image, then reload the
         * same binary into the same persistent-cache key.  The second image
         * therefore begins with patchable() restored from pcache v9.
         */
        char *child_argv[] = {argv[0], (char *)"warm", NULL};
        execve("/proc/self/exe", child_argv, environment);
        return 2;
    }

    size_t page_size = 4096;
    uintptr_t page = (uintptr_t)patchable & ~(uintptr_t)(page_size - 1);
    if (mprotect((void *)page, page_size,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return 3;
    uint32_t *instruction = (uint32_t *)(uintptr_t)patchable;
    instruction[0] = 0x528002c0u; /* movz w0,#22 */
    __builtin___clear_cache((char *)instruction, (char *)(instruction + 1));
    unsigned after = call_patchable();
    printf("pcache-smc before=%u after=%u\n", before, after);
    return before == 11 && after == 22 ? 0 : 1;
}

#endif
