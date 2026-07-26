#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "test.h"

#include "../../src/linux_abi/logical_vma.h"
#include "../../src/translator/guest_fetch.h"
#include "../../src/translator/guest_memory.h"

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int scratch_fd(void) {
    char path[] = "/tmp/hl-guest-fetch.XXXXXX";
    int descriptor = mkstemp(path);
    if (descriptor >= 0) unlink(path);
    return descriptor;
}

static uint64_t g_direct_first;
static uint64_t g_direct_last;
static int g_direct_enabled;

static int direct_fetch_valid(uint64_t address, size_t length) {
    return g_direct_enabled && address >= g_direct_first && address <= UINT64_MAX - length &&
           address + length <= g_direct_last;
}

// What core/target/*.c binds; the translator itself never names the ledger.
static const hl_guest_memory_ops g_ledger_ops = {hl_logical_vma_resolve_exec, NULL, NULL, NULL, NULL};

int main(void) {
    int descriptor = scratch_fd();
    HL_CHECK(descriptor >= 0);
    HL_CHECK(ftruncate(descriptor, 32768) == 0);

    unsigned char source[8192];
    for (size_t index = 0; index < sizeof source; ++index)
        source[index] = (unsigned char)(index * 29u + 7u);
    HL_CHECK(pwrite(descriptor, source, sizeof source, 4096) == (ssize_t)sizeof source);

    unsigned char *direct = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    HL_CHECK(direct != MAP_FAILED);
    for (size_t index = 0; index < 4 * 4096; ++index)
        direct[index] = (unsigned char)(index * 11u + 3u);
    const uint64_t guest = (uint64_t)(uintptr_t)(direct + 4096);
    g_direct_first = (uint64_t)(uintptr_t)direct;
    g_direct_last = g_direct_first + 4 * 4096;
    g_direct_enabled = 1;
    hl_guest_fetch_set_direct_validator(direct_fetch_valid);
    hl_guest_memory_bind(&g_ledger_ops);
    hl_logical_vma_global_reset_quiescent();
    HL_CHECK(hl_logical_vma_global_map_shared(guest, sizeof source, HL_LOGICAL_VMA_READ, descriptor, 4096, 16384) == 0);

    unsigned char fetched[15];
    /* Read permission is not execute permission. */
    HL_CHECK(hl_guest_fetch_exec(guest, fetched, sizeof fetched) == -1);

    HL_CHECK(hl_logical_vma_global_protect(guest, sizeof source, HL_LOGICAL_VMA_READ | HL_LOGICAL_VMA_EXEC) == 0);
    hl_logical_vma_global_reclaim_quiescent();
    HL_CHECK(hl_guest_fetch_exec(guest + 4090, fetched, sizeof fetched) == 0);
    HL_CHECK(memcmp(fetched, source + 4090, sizeof fetched) == 0);

    /* An instruction may legally cross between a direct mapping and a sparse
       logical mapping in either direction. */
    unsigned char expected[4] = {direct[4094], direct[4095], source[0], source[1]};
    HL_CHECK(hl_guest_fetch_exec(guest - 2, fetched, sizeof expected) == 0);
    HL_CHECK(memcmp(fetched, expected, sizeof expected) == 0);

    /* Removing the second logical page exposes the adjacent direct page. */
    HL_CHECK(hl_logical_vma_global_unmap(guest + 4096, 4096) == 0);
    hl_logical_vma_global_reclaim_quiescent();
    HL_CHECK(hl_guest_fetch_exec(guest + 4094, fetched, 2) == 0);
    expected[0] = source[4094];
    expected[1] = source[4095];
    expected[2] = direct[8192];
    expected[3] = direct[8193];
    HL_CHECK(hl_guest_fetch_exec(guest + 4094, fetched, sizeof expected) == 0);
    HL_CHECK(memcmp(fetched, expected, sizeof expected) == 0);

    /* The same transition faults when the direct peer is absent/PROT_NONE
       according to the target's authoritative direct-range validator. */
    g_direct_enabled = 0;
    HL_CHECK(hl_guest_fetch_exec(guest + 4094, fetched, 3) == -1);

    hl_logical_vma_global_reset_quiescent();
    hl_guest_fetch_set_direct_validator(NULL);
    HL_CHECK(munmap(direct, 4 * 4096) == 0);
    close(descriptor);
    return 0;
}
