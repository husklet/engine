/*
 * combined_bench.c - single-process, self-timing multi-phase benchmark.
 *
 * Runs several workload phases sequentially in ONE process. Each phase is
 * timed FROM INSIDE THE GUEST with the ARM generic timer (cntvct_el0 /
 * cntfrq_el0), so the reported per-phase microseconds reflect only in-guest
 * execution. Engine startup and worker fork are paid once, before any phase
 * runs, and are therefore excluded from every phase measurement.
 *
 * The per-phase logic is ported from tests/perf/{syscall,ops}.c so the work is
 * identical to the existing standalone perf guests.
 *
 * Output: one line per phase, e.g.  "PHASE compute us=1234 ok=1"
 * Deterministic, fixed iteration counts, each phase warmed before timing.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/* ---- in-guest cycle-counter timing (ARM generic timer) ---- */

static inline uint64_t timer_count(void) {
    uint64_t v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static inline uint64_t timer_freq(void) {
    uint64_t v;
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static uint64_t g_freq;

/* Convert an elapsed tick count to microseconds. */
static uint64_t ticks_to_us(uint64_t ticks) {
    return (ticks * UINT64_C(1000000)) / g_freq;
}

/* Sink to keep the optimizer from deleting computed results. */
static volatile uint64_t g_sink;

/* ---------------- phase: compute (integer + float busyloop) ---------------- */

static uint64_t phase_compute(void) {
    /* Ported shape of a busyloop: long dependency chain, int + float mix. */
    enum { N = 40000000 };
    uint64_t acc = 1469598103934665603ULL;
    double f = 1.0;
    for (int i = 0; i < N; ++i) {
        acc ^= (uint64_t)i;
        acc *= 1099511628211ULL;
        acc = (acc << 13) | (acc >> 51);
        f = f * 1.0000001 + 0.5;
        if (f > 1.0e6) f -= 1.0e6;
    }
    return acc ^ (uint64_t)f;
}

/* ---------------- phase: syscall (gettid loop, 1M) ---------------- */

static uint64_t phase_syscall(unsigned iters) {
    uint64_t checksum = 0;
    for (unsigned i = 0; i < iters; ++i)
        checksum += (uint64_t)syscall(SYS_gettid);
    return checksum;
}

/* ---------------- phase: mmap (map / touch / mprotect / unmap) ---------------- */

static uint64_t phase_mmap(unsigned iters) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uint64_t ok = 0;
    for (unsigned i = 0; i < iters; ++i) {
        unsigned char *p = mmap(NULL, page, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return ok;
        p[i % page] = (unsigned char)i;
        if (mprotect(p, page, PROT_READ) == 0 && p[i % page] == (unsigned char)i &&
            munmap(p, page) == 0)
            ok++;
    }
    return ok;
}

/* ---------------- phase: file (pwrite / pread loop) ---------------- */

static int full_ok_write(int fd, const void *buf, size_t n, off_t off) {
    return pwrite(fd, buf, n, off) == (ssize_t)n;
}

static uint64_t phase_file(unsigned iters) {
    char path[] = "/tmp/hl-combined-file-XXXXXX";
    unsigned char block[4096];
    int fd = mkstemp(path);
    if (fd < 0) return 0;
    (void)unlink(path);
    memset(block, 0x5a, sizeof(block));
    uint64_t ok = 0;
    for (unsigned i = 0; i < iters; ++i) {
        off_t offset = (off_t)(i % 256U) * (off_t)sizeof(block);
        if (!full_ok_write(fd, block, sizeof(block), offset)) break;
        if (pread(fd, block, sizeof(block), offset) == (ssize_t)sizeof(block) &&
            block[0] == 0x5a)
            ok++;
    }
    (void)close(fd);
    return ok;
}

/* ---------------- phase: pipe (write/read round-trip) ---------------- */

static int full_write(int fd, const void *buffer, size_t size) {
    const unsigned char *cursor = buffer;
    while (size != 0) {
        ssize_t written = write(fd, cursor, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        cursor += (size_t)written;
        size -= (size_t)written;
    }
    return 0;
}

static int full_read(int fd, void *buffer, size_t size) {
    unsigned char *cursor = buffer;
    while (size != 0) {
        ssize_t got = read(fd, cursor, size);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return -1;
        cursor += (size_t)got;
        size -= (size_t)got;
    }
    return 0;
}

static uint64_t phase_pipe(unsigned iters) {
    int fds[2];
    uint64_t value = UINT64_C(0x123456789abcdef0);
    uint64_t ok = 0;
    if (pipe(fds) != 0) return 0;
    for (unsigned i = 0; i < iters; ++i) {
        if (full_write(fds[1], &value, sizeof(value)) != 0 ||
            full_read(fds[0], &value, sizeof(value)) != 0)
            break;
        ok++;
    }
    (void)close(fds[0]);
    (void)close(fds[1]);
    return ok;
}

/* ---------------- phase: memory (memcpy + strcmp bandwidth) ---------------- */

static uint64_t phase_memory(unsigned iters) {
    enum { BUF = 1 << 20 }; /* 1 MiB */
    unsigned char *a = malloc(BUF);
    unsigned char *b = malloc(BUF);
    if (a == NULL || b == NULL) {
        free(a);
        free(b);
        return 0;
    }
    memset(a, 0x5a, BUF);
    memset(b, 0x00, BUF);
    uint64_t acc = 0;
    for (unsigned i = 0; i < iters; ++i) {
        memcpy(b, a, BUF);
        b[i % BUF] = (unsigned char)i; /* defeat memcpy elision */
        acc += (uint64_t)memcmp(a, b, BUF);
        acc += b[(i * 7) % BUF];
    }
    free(a);
    free(b);
    return acc;
}

/* ---------------- driver ---------------- */

static uint64_t run_phase(const char *name, uint64_t (*fn)(unsigned), unsigned iters,
                          unsigned warm) {
    /* Warm: a few throwaway iterations so translation/cache is primed. */
    for (unsigned w = 0; w < warm; ++w) g_sink += fn(iters / 20 + 1);
    uint64_t t0 = timer_count();
    uint64_t r = fn(iters);
    uint64_t t1 = timer_count();
    g_sink += r;
    uint64_t us = ticks_to_us(t1 - t0);
    printf("PHASE %s us=%llu ok=%llu\n", name, (unsigned long long)us,
           (unsigned long long)r);
    fflush(stdout);
    return us;
}

/* compute takes no iters arg; wrap it for the uniform signature. */
static uint64_t compute_wrap(unsigned iters) {
    (void)iters;
    return phase_compute();
}

int main(void) {
    g_freq = timer_freq();
    if (g_freq == 0) {
        fprintf(stderr, "cntfrq_el0 is zero\n");
        return 1;
    }
    printf("cntfrq=%llu\n", (unsigned long long)g_freq);
    fflush(stdout);

    /* compute appears twice to expose cold-translation vs warm delta. */
    run_phase("compute_cold", compute_wrap, 0, 0);
    run_phase("compute", compute_wrap, 0, 1);
    run_phase("syscall", phase_syscall, 1000000, 2);
    run_phase("mmap", phase_mmap, 10000, 2);
    run_phase("file", phase_file, 4096, 2);
    run_phase("pipe", phase_pipe, 100000, 2);
    run_phase("memory", phase_memory, 256, 2);
    return 0;
}
