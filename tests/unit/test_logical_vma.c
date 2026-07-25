#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "test.h"

#include "../../src/linux_abi/logical_vma.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct lookup_stress {
    hl_logical_vma_ledger *ledger;
    uint64_t address;
    _Atomic int stop;
    _Atomic unsigned long lookups;
    _Atomic int failed;
} lookup_stress;

static void *lookup_thread(void *opaque) {
    lookup_stress *stress = opaque;
    while (!atomic_load_explicit(&stress->stop, memory_order_acquire)) {
        hl_logical_vma_resolution resolution;
        int result = hl_logical_vma_resolve(stress->ledger, stress->address, 1, HL_LOGICAL_VMA_READ, &resolution);
        if (result != 1 || resolution.host == NULL || resolution.contiguous == 0) {
            atomic_store_explicit(&stress->failed, 1, memory_order_release);
            break;
        }
        (void)*(volatile unsigned char *)resolution.host;
        atomic_fetch_add_explicit(&stress->lookups, 1, memory_order_relaxed);
    }
    return NULL;
}

typedef struct pin_stress {
    uint64_t address;
    _Atomic int stop;
    _Atomic int failed;
    _Atomic unsigned long pins;
} pin_stress;

static void *pin_thread(void *opaque) {
    pin_stress *stress = opaque;
    while (!atomic_load_explicit(&stress->stop, memory_order_acquire)) {
        hl_logical_vma_pin pin;
        if (hl_logical_vma_pin_data(stress->address, 1, HL_LOGICAL_VMA_READ, &pin) != 1) {
            atomic_store_explicit(&stress->failed, 1, memory_order_release);
            break;
        }
        (void)*(volatile unsigned char *)pin.host;
        hl_logical_vma_unpin(&pin);
        atomic_fetch_add_explicit(&stress->pins, 1, memory_order_relaxed);
    }
    return NULL;
}

int main(void) {
    hl_logical_vma_ledger ledger;
    HL_CHECK(hl_logical_vma_init(&ledger) == 0);
    char path[] = "/tmp/hl-logical-vma.XXXXXX";
    int fd = mkstemp(path);
    HL_CHECK(fd >= 0);
    HL_CHECK(ftruncate(fd, 65536) == 0);
    int readonly_fd = open(path, O_RDONLY);
    HL_CHECK(readonly_fd >= 0);
    unlink(path);

    const uint64_t rw = UINT64_C(0x500000004000);
    const uint64_t rx = UINT64_C(0x500000014000);
    hl_logical_vma_resolution alias_view;
    /* A read/execute shared view must not require write access to its fd. */
    HL_CHECK(hl_logical_vma_map_shared(&ledger, rx, 12288, HL_LOGICAL_VMA_READ | HL_LOGICAL_VMA_EXEC, readonly_fd, 4096,
                                       16384) == 0);
    HL_CHECK(hl_logical_vma_map_shared(&ledger, rw, 12288, HL_LOGICAL_VMA_READ | HL_LOGICAL_VMA_WRITE, fd, 4096,
                                       16384) == 0);
    HL_CHECK(hl_logical_vma_count(&ledger) == 2);

    /* A host mmap failure must leave both ledger contents and its mutation
       mutex usable.  This catches cleanup paths that unlock the ledger twice. */
    int directory_fd = open("/tmp", O_RDONLY);
    HL_CHECK(directory_fd >= 0);
    errno = 0;
    HL_CHECK(hl_logical_vma_map_shared(&ledger, UINT64_C(0x500000054000), 4096,
                                       HL_LOGICAL_VMA_READ, directory_fd, 0, 16384) == -1);
    close(directory_fd);
    HL_CHECK(hl_logical_vma_count(&ledger) == 2);
    HL_CHECK(hl_logical_vma_resolve(&ledger, rw, 1, HL_LOGICAL_VMA_READ,
                                    &alias_view) == 1);

    hl_logical_vma_pin overflow_pin;
    errno = 0;
    HL_CHECK(hl_logical_vma_pin_data(UINT64_MAX - 3, 8, HL_LOGICAL_VMA_READ,
                                     &overflow_pin) == -1);
    HL_CHECK(errno == EINVAL);

    hl_logical_vma_resolution write_view, exec_view;
    HL_CHECK(hl_logical_vma_resolve(&ledger, rw + 4092, 16, HL_LOGICAL_VMA_WRITE, &write_view) == 1);
    HL_CHECK(hl_logical_vma_resolve(&ledger, rx + 4092, 16, HL_LOGICAL_VMA_EXEC, &exec_view) == 1);
    static const unsigned char patch[16] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x10,
    };
    memcpy(write_view.host, patch, sizeof(patch));
    HL_CHECK(memcmp(exec_view.host, patch, sizeof(patch)) == 0);
    HL_CHECK(exec_view.host == write_view.host);
    close(readonly_fd);

    /* Preparation failure and abort are invisible: no interval, canonical
       pointer, or backing byte changes before the irreversible host mmap. */
    const uint64_t transactional = UINT64_C(0x500000044000);
    hl_logical_vma_test_fail_next_prepare();
    hl_logical_vma_plan *plan = NULL;
    errno = 0;
    HL_CHECK(hl_logical_vma_prepare_shared(&ledger, transactional, 4096, HL_LOGICAL_VMA_READ, fd, 0, 16384, &plan) ==
             -1);
    HL_CHECK(errno == ENOMEM && plan == NULL);
    HL_CHECK(hl_logical_vma_count(&ledger) == 2);
    HL_CHECK(hl_logical_vma_resolve(&ledger, transactional, 1, HL_LOGICAL_VMA_READ, &alias_view) == 0);
    HL_CHECK(hl_logical_vma_prepare_shared(&ledger, transactional, 4096, HL_LOGICAL_VMA_READ, fd, 0, 16384, &plan) ==
             0);
    hl_logical_vma_abort_shared(plan);
    HL_CHECK(hl_logical_vma_count(&ledger) == 2);
    HL_CHECK(hl_logical_vma_resolve(&ledger, transactional, 1, HL_LOGICAL_VMA_READ, &alias_view) == 0);

    /* A separately acquired fd for the same vnode still selects one canonical
       address for the same file bytes. */
    int fd2 = dup(fd);
    HL_CHECK(fd2 >= 0);
    const uint64_t alias = UINT64_C(0x500000024000);
    HL_CHECK(hl_logical_vma_map_shared(&ledger, alias, 12288, HL_LOGICAL_VMA_READ, fd2, 4096, 16384) == 0);
    HL_CHECK(hl_logical_vma_resolve(&ledger, alias + 4092, 16, HL_LOGICAL_VMA_READ, &alias_view) == 1);
    HL_CHECK(hl_logical_vma_resolve(&ledger, rx + 4092, 16, HL_LOGICAL_VMA_EXEC, &exec_view) == 1);
    HL_CHECK(alias_view.host == exec_view.host);
    close(fd2);
    plan = NULL;
    HL_CHECK(hl_logical_vma_prepare_protect(&ledger, alias + 4096, 4096, HL_LOGICAL_VMA_READ | HL_LOGICAL_VMA_EXEC,
                                            &plan) == 0);
    /* Prepare is invisible and abort preserves the old permissions. */
    HL_CHECK(hl_logical_vma_resolve(&ledger, alias + 4096, 1, HL_LOGICAL_VMA_EXEC, &alias_view) == -1);
    hl_logical_vma_abort_shared(plan);
    HL_CHECK(hl_logical_vma_resolve(&ledger, alias + 4096, 1, HL_LOGICAL_VMA_EXEC, &alias_view) == -1);
    HL_CHECK(hl_logical_vma_prepare_protect(&ledger, alias + 4096, 4096, HL_LOGICAL_VMA_READ | HL_LOGICAL_VMA_EXEC,
                                            &plan) == 0);
    hl_logical_vma_commit_shared(plan);
    hl_logical_vma_reclaim_quiescent(&ledger);
    HL_CHECK(hl_logical_vma_resolve(&ledger, alias + 4096, 1, HL_LOGICAL_VMA_EXEC, &alias_view) == 1);
    HL_CHECK(hl_logical_vma_resolve(&ledger, alias, 1, HL_LOGICAL_VMA_EXEC, &alias_view) == -1);

    /* Removing the middle logical page splits without losing either peer. */
    HL_CHECK(hl_logical_vma_unmap(&ledger, rw + 4096, 4096) == 0);
    hl_logical_vma_reclaim_quiescent(&ledger);
    HL_CHECK(hl_logical_vma_count(&ledger) == 6);
    HL_CHECK(hl_logical_vma_resolve(&ledger, rw, 1, HL_LOGICAL_VMA_READ, &write_view) == 1);
    HL_CHECK(hl_logical_vma_resolve(&ledger, rw + 4096, 1, HL_LOGICAL_VMA_READ, &write_view) == 0);
    HL_CHECK(hl_logical_vma_resolve(&ledger, rw + 8192, 1, HL_LOGICAL_VMA_READ, &write_view) == 1);

    /* MAP_FIXED-style replacement retires only overlapping logical coverage. */
    HL_CHECK(hl_logical_vma_map_shared(&ledger, rw + 4096, 4096, HL_LOGICAL_VMA_READ | HL_LOGICAL_VMA_WRITE, fd, 32768,
                                       16384) == 0);
    hl_logical_vma_reclaim_quiescent(&ledger);
    HL_CHECK(hl_logical_vma_count(&ledger) == 7);
    HL_CHECK(hl_logical_vma_resolve(&ledger, rw + 4096, 1, HL_LOGICAL_VMA_WRITE, &write_view) == 1);
    *(unsigned char *)write_view.host = 0x5a;
    unsigned char byte = 0;
    HL_CHECK(pread(fd, &byte, 1, 32768) == 1);
    HL_CHECK(byte == 0x5a);

    /* Immutable snapshots remain readable while a writer repeatedly replaces
       the same logical page. Reclamation is deliberately delayed until every
       reader has crossed the test's quiescent point (join). */
    const uint64_t hot = UINT64_C(0x500000034000);
    HL_CHECK(hl_logical_vma_map_shared(&ledger, hot, 4096, HL_LOGICAL_VMA_READ, fd, 0, 16384) == 0);
    hl_logical_vma_resolution retained;
    HL_CHECK(hl_logical_vma_resolve(&ledger, hot, 1, HL_LOGICAL_VMA_READ, &retained) == 1);
    lookup_stress stress = {.ledger = &ledger, .address = hot};
    pthread_t readers[4];
    for (size_t index = 0; index < 4; ++index)
        HL_CHECK(pthread_create(&readers[index], NULL, lookup_thread, &stress) == 0);
    for (unsigned iteration = 0; iteration < 2000; ++iteration) {
        uint64_t offset = (iteration & 1u) ? 16384u : 0u;
        HL_CHECK(hl_logical_vma_map_shared(&ledger, hot, 4096, HL_LOGICAL_VMA_READ, fd, offset, 16384) == 0);
    }
    /* The pre-replacement pointer is still backed before quiescent reclaim. */
    (void)*(volatile unsigned char *)retained.host;
    atomic_store_explicit(&stress.stop, 1, memory_order_release);
    for (size_t index = 0; index < 4; ++index)
        HL_CHECK(pthread_join(readers[index], NULL) == 0);
    HL_CHECK(!atomic_load_explicit(&stress.failed, memory_order_acquire));
    HL_CHECK(atomic_load_explicit(&stress.lookups, memory_order_relaxed) != 0);
    hl_logical_vma_reclaim_quiescent(&ledger);

    /* A blocking syscall may retain a canonical pointer while a mapping peer
       replaces and reclaims snapshots. The explicit backing pin must keep the
       hidden mapping alive independently of snapshot lifetime. */
    const uint64_t pinned = UINT64_C(0x600000004000);
    HL_CHECK(hl_logical_vma_global_map_shared(pinned, 4096, HL_LOGICAL_VMA_READ, fd, 0, 16384) == 0);
    pin_stress pstress = {.address = pinned};
    pthread_t pinner;
    HL_CHECK(pthread_create(&pinner, NULL, pin_thread, &pstress) == 0);
    for (unsigned iteration = 0; iteration < 2000; ++iteration) {
        uint64_t offset = (iteration & 1u) ? 16384u : 0u;
        HL_CHECK(hl_logical_vma_global_map_shared(pinned, 4096, HL_LOGICAL_VMA_READ, fd, offset, 16384) == 0);
        hl_logical_vma_global_reclaim_quiescent();
    }
    atomic_store_explicit(&pstress.stop, 1, memory_order_release);
    HL_CHECK(pthread_join(pinner, NULL) == 0);
    HL_CHECK(!atomic_load_explicit(&pstress.failed, memory_order_acquire));
    HL_CHECK(atomic_load_explicit(&pstress.pins, memory_order_relaxed) != 0);
    hl_logical_vma_global_reset_quiescent();

    /*
     * Checkpoint primitives preserve absolute vnode offsets, guest protection,
     * and RW/RX alias identity.  The payload straddles a logical 4 KiB edge,
     * while the simulated host granularity is 16 KiB.
     */
    const uint64_t checkpoint_rw = UINT64_C(0x600000104000);
    const uint64_t checkpoint_rx = UINT64_C(0x600000204000);
    HL_CHECK(hl_logical_vma_global_restore_shared(
                 checkpoint_rx, 8192, HL_LOGICAL_VMA_READ | HL_LOGICAL_VMA_EXEC, fd, 4096, 16384) == 0);
    HL_CHECK(hl_logical_vma_global_restore_shared(
                 checkpoint_rw, 8192, HL_LOGICAL_VMA_READ | HL_LOGICAL_VMA_WRITE, fd, 4096, 16384) == 0);
    hl_logical_vma_descriptor checkpoint_descriptors[2];
    HL_CHECK(hl_logical_vma_global_export(checkpoint_descriptors, 2) == 2);
    hl_logical_vma_descriptor checkpoint_descriptor;
    HL_CHECK(hl_logical_vma_global_describe(checkpoint_rx + 4096, &checkpoint_descriptor) == 1);
    HL_CHECK(checkpoint_descriptor.guest_first == checkpoint_rx);
    HL_CHECK(checkpoint_descriptor.backing_offset == 4096);
    HL_CHECK(checkpoint_descriptor.protection == (HL_LOGICAL_VMA_READ | HL_LOGICAL_VMA_EXEC));
    static const unsigned char checkpoint_patch[16] = {
        0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x17, 0x28, 0x39, 0x4a, 0x5b, 0x6c, 0x7d, 0x8e, 0x9f, 0x10,
    };
    HL_CHECK(hl_logical_vma_global_copy_in(checkpoint_rw + 4092, checkpoint_patch,
                                           sizeof checkpoint_patch) == 0);
    unsigned char checkpoint_readback[sizeof checkpoint_patch];
    HL_CHECK(hl_logical_vma_global_copy_out(checkpoint_rx + 4092, checkpoint_readback,
                                            sizeof checkpoint_readback) == 0);
    HL_CHECK(memcmp(checkpoint_readback, checkpoint_patch, sizeof checkpoint_patch) == 0);
    hl_logical_vma_pin checkpoint_pin;
    HL_CHECK(hl_logical_vma_pin_data(checkpoint_rx, 1, HL_LOGICAL_VMA_WRITE, &checkpoint_pin) == -1);
    HL_CHECK(errno == EACCES);
    hl_logical_vma_global_reset_quiescent();

    hl_logical_vma_destroy(&ledger);
    close(fd);
    return 0;
}
