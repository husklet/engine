#define _GNU_SOURCE
#include "system.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define HL_PRIVATE_PROCESSES 1024u
#define HL_PRIVATE_CELLS 256u
#define HL_PRIVATE_INIT 1u
#define HL_PRIVATE_LIVE 2u

typedef struct hl_private_process {
    _Atomic uint64_t claim;
    _Atomic uint64_t claim_start_ns;
    _Atomic uint32_t state;
    _Atomic int64_t pid;
    _Atomic uint64_t start_ns;
    _Atomic uint64_t generation;
    _Atomic uint64_t cells[HL_PRIVATE_CELLS]; /* high32=fd+1, low32=references */
} hl_private_process;

static hl_private_process *hl_private;
static _Atomic uint64_t *hl_private_epoch;
static _Thread_local int64_t hl_private_pid;
static _Thread_local uint64_t hl_private_start;
static uint64_t *hl_private_fork_cells;
static size_t hl_private_fork_count;
static pthread_mutex_t hl_private_fork_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t hl_private_process_start(int64_t pid) {
    hl_host_process_info info;
    return hl_host_process_read(pid, &info) ? info.start_time_ns : 0;
}

static uint64_t hl_private_identity(int64_t pid, uint64_t start) {
    return ((uint64_t)(uint32_t)pid << 32) | ((uint32_t)start ^ (uint32_t)(start >> 32));
}

static void hl_private_cells_clear(hl_private_process *process) {
    for (unsigned index = 0; index < HL_PRIVATE_CELLS; ++index)
        atomic_store_explicit(&process->cells[index], 0, memory_order_relaxed);
}

static hl_private_process *hl_private_claim(int64_t pid, uint64_t start) {
    for (unsigned index = 0; index < HL_PRIVATE_PROCESSES; ++index) {
        hl_private_process *process = &hl_private[index];
        uint32_t state = atomic_load_explicit(&process->state, memory_order_acquire);
        if (state == HL_PRIVATE_INIT) {
            uint64_t claim = atomic_load_explicit(&process->claim, memory_order_acquire);
            int64_t claim_pid = (int64_t)(uint32_t)(claim >> 32);
            uint64_t live = hl_private_process_start(claim_pid);
            uint64_t claim_start = atomic_load_explicit(&process->claim_start_ns, memory_order_acquire);
            if (claim != 0 && live != 0 && live == claim_start) continue;
            atomic_store_explicit(&process->claim_start_ns, 0, memory_order_relaxed);
            uint64_t stale = claim;
            if (!atomic_compare_exchange_strong_explicit(&process->claim, &stale, 0, memory_order_acq_rel,
                                                         memory_order_relaxed))
                continue;
            atomic_store_explicit(&process->state, 0, memory_order_release);
            state = 0;
        }
        if (state == 0) {
            uint64_t claim = atomic_load_explicit(&process->claim, memory_order_acquire);
            if (claim != 0) {
                int64_t claim_pid = (int64_t)(uint32_t)(claim >> 32);
                uint64_t live = hl_private_process_start(claim_pid);
                if (live != 0 && hl_private_identity(claim_pid, live) == claim) continue;
                uint64_t stale = claim;
                if (!atomic_compare_exchange_strong_explicit(&process->claim, &stale, 0, memory_order_acq_rel,
                                                             memory_order_relaxed))
                    continue;
            }
        }
        if (state == HL_PRIVATE_LIVE) {
            int64_t record_pid = atomic_load_explicit(&process->pid, memory_order_relaxed);
            uint64_t record_start = atomic_load_explicit(&process->start_ns, memory_order_relaxed);
            uint64_t live_start = hl_private_process_start(record_pid);
            if (live_start != 0 && live_start == record_start) continue;
            uint32_t expected = HL_PRIVATE_LIVE;
            if (!atomic_compare_exchange_strong_explicit(&process->state, &expected, HL_PRIVATE_INIT,
                                                         memory_order_acq_rel, memory_order_relaxed))
                continue;
            hl_private_cells_clear(process);
            atomic_store_explicit(&process->claim_start_ns, 0, memory_order_relaxed);
            atomic_store_explicit(&process->claim, 0, memory_order_relaxed);
            atomic_store_explicit(&process->state, 0, memory_order_release);
            state = 0;
        }
        if (state != 0) continue;
        uint64_t empty_claim = 0;
        uint64_t mine = hl_private_identity(pid, start);
        if (!atomic_compare_exchange_strong_explicit(&process->claim, &empty_claim, mine, memory_order_acq_rel,
                                                     memory_order_relaxed))
            continue;
        atomic_store_explicit(&process->claim_start_ns, start, memory_order_relaxed);
        atomic_store_explicit(&process->state, HL_PRIVATE_INIT, memory_order_release);
        atomic_store_explicit(&process->pid, pid, memory_order_relaxed);
        atomic_store_explicit(&process->start_ns, start, memory_order_relaxed);
        hl_private_cells_clear(process);
        atomic_store_explicit(&process->state, HL_PRIVATE_LIVE, memory_order_release);
        return process;
    }
    return NULL;
}

static uint64_t hl_private_cell(int fd, uint32_t references) {
    return ((uint64_t)(uint32_t)(fd + 1) << 32) | references;
}

static void hl_private_cleanup(void) {
    int64_t pid = (int64_t)getpid();
    uint64_t start = hl_private_process_start(pid);
    for (unsigned index = 0; index < HL_PRIVATE_PROCESSES; ++index) {
        hl_private_process *process = &hl_private[index];
        if (atomic_load_explicit(&process->state, memory_order_acquire) != HL_PRIVATE_LIVE ||
            atomic_load_explicit(&process->pid, memory_order_relaxed) != pid ||
            atomic_load_explicit(&process->start_ns, memory_order_relaxed) != start)
            continue;
        uint32_t expected = HL_PRIVATE_LIVE;
        if (!atomic_compare_exchange_strong_explicit(&process->state, &expected, HL_PRIVATE_INIT, memory_order_acq_rel,
                                                     memory_order_relaxed))
            continue;
        hl_private_cells_clear(process);
        atomic_store_explicit(&process->claim_start_ns, 0, memory_order_relaxed);
        atomic_store_explicit(&process->claim, 0, memory_order_relaxed);
        atomic_store_explicit(&process->state, 0, memory_order_release);
    }
}

void hl_host_private_init(void) {
    size_t records_size = sizeof(*hl_private) * HL_PRIVATE_PROCESSES;
    if (hl_private != NULL) return;
    void *memory =
        mmap(NULL, records_size + sizeof(*hl_private_epoch), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory != MAP_FAILED) {
        hl_private = memory;
        hl_private_epoch = (void *)((unsigned char *)memory + records_size);
        (void)atexit(hl_private_cleanup);
    }
}

#ifndef HL_EMBEDDED_BUILD
__attribute__((constructor)) static void hl_private_constructor(void) {
    hl_host_private_init();
}
#endif

static int hl_private_add_unlocked(int fd) {
    int64_t pid = (int64_t)getpid();
    uint64_t start = hl_private_process_start(pid);
    if (hl_private_pid != pid || hl_private_start != start) {
        hl_private_pid = pid;
        hl_private_start = start;
    }
    if (!hl_private || fd < 0 || fd == INT32_MAX || start == 0) return -ENOSPC;
    for (unsigned record = 0; record < HL_PRIVATE_PROCESSES; ++record) {
        hl_private_process *process = &hl_private[record];
        if (atomic_load_explicit(&process->state, memory_order_acquire) != HL_PRIVATE_LIVE ||
            atomic_load_explicit(&process->pid, memory_order_relaxed) != pid ||
            atomic_load_explicit(&process->start_ns, memory_order_relaxed) != start)
            continue;
        for (unsigned index = 0; index < HL_PRIVATE_CELLS; ++index) {
            uint64_t value = atomic_load_explicit(&process->cells[index], memory_order_acquire);
            if ((uint32_t)(value >> 32) == (uint32_t)(fd + 1)) {
                for (;;) {
                    uint32_t references = (uint32_t)value;
                    if (references == UINT32_MAX) return -EOVERFLOW;
                    uint64_t next = hl_private_cell(fd, references + 1);
                    if (atomic_compare_exchange_weak_explicit(&process->cells[index], &value, next,
                                                              memory_order_acq_rel, memory_order_relaxed)) {
                        atomic_fetch_add_explicit(&process->generation, 1, memory_order_release);
                        atomic_fetch_add_explicit(hl_private_epoch, 1, memory_order_release);
                        return 0;
                    }
                }
            }
            if (value == 0) {
                uint64_t empty = 0;
                if (atomic_compare_exchange_strong_explicit(&process->cells[index], &empty, hl_private_cell(fd, 1),
                                                            memory_order_acq_rel, memory_order_relaxed)) {
                    atomic_fetch_add_explicit(&process->generation, 1, memory_order_release);
                    atomic_fetch_add_explicit(hl_private_epoch, 1, memory_order_release);
                    return 0;
                }
            }
        }
    }
    hl_private_process *process = hl_private_claim(pid, start);
    if (!process) return -ENOSPC;
    atomic_store_explicit(&process->cells[0], hl_private_cell(fd, 1), memory_order_release);
    atomic_fetch_add_explicit(&process->generation, 1, memory_order_release);
    atomic_fetch_add_explicit(hl_private_epoch, 1, memory_order_release);
    return 0;
}

int hl_host_process_fd_private_add(int fd) {
    int result;
    if (pthread_mutex_lock(&hl_private_fork_lock) != 0) return -EDEADLK;
    result = hl_private_add_unlocked(fd);
    (void)pthread_mutex_unlock(&hl_private_fork_lock);
    return result;
}

static void hl_private_remove_unlocked(int fd) {
    int64_t pid = (int64_t)getpid();
    uint64_t start = hl_private_process_start(pid);
    if (!hl_private || fd < 0) return;
    for (unsigned record = 0; record < HL_PRIVATE_PROCESSES; ++record) {
        hl_private_process *process = &hl_private[record];
        if (atomic_load_explicit(&process->state, memory_order_acquire) != HL_PRIVATE_LIVE ||
            atomic_load_explicit(&process->pid, memory_order_relaxed) != pid ||
            atomic_load_explicit(&process->start_ns, memory_order_relaxed) != start)
            continue;
        for (unsigned index = 0; index < HL_PRIVATE_CELLS; ++index) {
            uint64_t value = atomic_load_explicit(&process->cells[index], memory_order_acquire);
            if ((uint32_t)(value >> 32) != (uint32_t)(fd + 1)) continue;
            for (;;) {
                uint32_t references = (uint32_t)value;
                uint64_t next = references > 1 ? hl_private_cell(fd, references - 1) : 0;
                if (atomic_compare_exchange_weak_explicit(&process->cells[index], &value, next, memory_order_acq_rel,
                                                          memory_order_relaxed)) {
                    atomic_fetch_add_explicit(&process->generation, 1, memory_order_release);
                    atomic_fetch_add_explicit(hl_private_epoch, 1, memory_order_release);
                    return;
                }
                if ((uint32_t)(value >> 32) != (uint32_t)(fd + 1)) return;
            }
        }
    }
}

void hl_host_process_fd_private_remove(int fd) {
    if (pthread_mutex_lock(&hl_private_fork_lock) != 0) return;
    hl_private_remove_unlocked(fd);
    (void)pthread_mutex_unlock(&hl_private_fork_lock);
}

int hl_host_process_fd_private_is(int64_t pid, uint64_t start_ns, int fd) {
    if (!hl_private || fd < 0) return 0;
    for (unsigned record = 0; record < HL_PRIVATE_PROCESSES; ++record) {
        hl_private_process *process = &hl_private[record];
        if (atomic_load_explicit(&process->state, memory_order_acquire) != HL_PRIVATE_LIVE ||
            atomic_load_explicit(&process->pid, memory_order_relaxed) != pid ||
            atomic_load_explicit(&process->start_ns, memory_order_relaxed) != start_ns)
            continue;
        for (unsigned index = 0; index < HL_PRIVATE_CELLS; ++index) {
            uint64_t value = atomic_load_explicit(&process->cells[index], memory_order_acquire);
            if ((uint32_t)(value >> 32) == (uint32_t)(fd + 1) && (uint32_t)value != 0) {
                if (atomic_load_explicit(&process->state, memory_order_acquire) == HL_PRIVATE_LIVE &&
                    atomic_load_explicit(&process->pid, memory_order_relaxed) == pid &&
                    atomic_load_explicit(&process->start_ns, memory_order_relaxed) == start_ns)
                    return 1;
                break;
            }
        }
    }
    return 0;
}

int hl_host_process_fd_private_fork_prepare(void) {
    int64_t pid = (int64_t)getpid();
    uint64_t start = hl_private_process_start(pid);
    if (pthread_mutex_lock(&hl_private_fork_lock) != 0) return -EDEADLK;
    free(hl_private_fork_cells);
    hl_private_fork_cells = NULL;
    hl_private_fork_count = 0;
    size_t capacity = 0;
    /* Only this process's row belongs to the fork snapshot.  Other engine
       processes legitimately mutate the shared registry at any time; a
       registry-wide epoch check made those unrelated changes surface as a
       spurious guest fork EAGAIN.  The row generation and identity checks
       below detect every mutation that can affect this snapshot, while the
       process-local fork lock serializes this process's own descriptor
       mutations until fork_complete. */
    for (unsigned record = 0; record < HL_PRIVATE_PROCESSES; ++record) {
        hl_private_process *process = &hl_private[record];
        if (atomic_load_explicit(&process->state, memory_order_acquire) != HL_PRIVATE_LIVE ||
            atomic_load_explicit(&process->pid, memory_order_relaxed) != pid ||
            atomic_load_explicit(&process->start_ns, memory_order_relaxed) != start)
            continue;
        uint64_t generation = atomic_load_explicit(&process->generation, memory_order_acquire);
        for (unsigned index = 0; index < HL_PRIVATE_CELLS; ++index) {
            uint64_t cell = atomic_load_explicit(&process->cells[index], memory_order_acquire);
            if (cell == 0) continue;
            if (hl_private_fork_count == capacity) {
                size_t next = capacity ? capacity * 2 : 64;
                uint64_t *grown = realloc(hl_private_fork_cells, next * sizeof *grown);
                if (!grown) {
                    free(hl_private_fork_cells);
                    hl_private_fork_cells = NULL;
                    hl_private_fork_count = 0;
                    (void)pthread_mutex_unlock(&hl_private_fork_lock);
                    return -ENOMEM;
                }
                hl_private_fork_cells = grown;
                capacity = next;
            }
            hl_private_fork_cells[hl_private_fork_count++] = cell;
        }
        if (atomic_load_explicit(&process->generation, memory_order_acquire) != generation ||
            atomic_load_explicit(&process->state, memory_order_acquire) != HL_PRIVATE_LIVE ||
            atomic_load_explicit(&process->pid, memory_order_relaxed) != pid ||
            atomic_load_explicit(&process->start_ns, memory_order_relaxed) != start) {
            free(hl_private_fork_cells);
            hl_private_fork_cells = NULL;
            hl_private_fork_count = 0;
            (void)pthread_mutex_unlock(&hl_private_fork_lock);
            return -EAGAIN;
        }
    }
    hl_private_pid = pid;
    hl_private_start = start;
    return 0;
}

int hl_host_process_fd_private_fork_complete(int child) {
    int result = 0;
    if (child) {
        hl_private_pid = (int64_t)getpid();
        hl_private_start = hl_private_process_start(hl_private_pid);
        for (size_t index = 0; index < hl_private_fork_count; ++index) {
            int fd = (int)((uint32_t)(hl_private_fork_cells[index] >> 32) - 1u);
            uint32_t references = (uint32_t)hl_private_fork_cells[index];
            for (uint32_t reference = 0; reference < references; ++reference) {
                result = hl_private_add_unlocked(fd);
                if (result != 0) break;
            }
            if (result != 0) break;
        }
        if (result != 0) hl_private_cleanup();
    }
    free(hl_private_fork_cells);
    hl_private_fork_cells = NULL;
    hl_private_fork_count = 0;
    (void)pthread_mutex_unlock(&hl_private_fork_lock);
    return result;
}

void hl_host_process_fd_private_cleanup(void) {
    hl_private_cleanup();
}
