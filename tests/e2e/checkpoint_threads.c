#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static atomic_int ready;
static atomic_int stop;
static atomic_int failed;
static atomic_ulong progress;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int release_waiter;
static __thread int tls_value;
static long tids[3];
static atomic_int *process_ready;

static void *busy_worker(void *unused) {
    (void)unused;
    tls_value = 0x1357;
    tids[0] = syscall(SYS_gettid);
    atomic_fetch_add(&ready, 1);
    while (!atomic_load(&stop)) atomic_fetch_add_explicit(&progress, 1, memory_order_relaxed);
    if (tls_value != 0x1357 || syscall(SYS_gettid) != tids[0]) atomic_store(&failed, 1);
    return NULL;
}

static void *waiting_worker(void *unused) {
    (void)unused;
    tls_value = 0x2468;
    tids[1] = syscall(SYS_gettid);
    atomic_fetch_add(&ready, 1);
    pthread_mutex_lock(&mutex);
    while (!release_waiter) pthread_cond_wait(&condition, &mutex);
    pthread_mutex_unlock(&mutex);
    if (tls_value != 0x2468 || syscall(SYS_gettid) != tids[1]) atomic_store(&failed, 1);
    return NULL;
}

static void *new_worker(void *unused) {
    (void)unused;
    tls_value = 0x369c;
    tids[2] = syscall(SYS_gettid);
    if (tls_value != 0x369c || tids[2] == tids[0] || tids[2] == tids[1]) atomic_store(&failed, 1);
    return NULL;
}

static int prepare_output(const char *release) {
    char path[1024];
    if (snprintf(path, sizeof path, "%s.output", release) >= (int)sizeof path) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    if (snprintf(path, sizeof path, "%s.error", release) >= (int)sizeof path) return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0 || dup2(fd, STDERR_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    fd = open("/dev/null", O_RDONLY);
    if (fd < 0 || dup2(fd, STDIN_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    return 0;
}

static int run_process(const char *release, int coordinator) {
    pthread_t busy, waiting, fresh;
    if (pthread_create(&busy, NULL, busy_worker, NULL) != 0 ||
        pthread_create(&waiting, NULL, waiting_worker, NULL) != 0)
        return 3;
    while (atomic_load(&ready) != 2) sched_yield();
    unsigned long before = atomic_load(&progress);
    atomic_fetch_add(process_ready, 1);
    if (coordinator) {
        while (atomic_load(process_ready) != 2) sched_yield();
        dprintf(STDOUT_FILENO, "READY 1\n");
    }
    while (access(release, F_OK) != 0)
        if (errno != ENOENT) return 4;
    while (atomic_load(&progress) == before) sched_yield();
    atomic_store(&stop, 1);
    pthread_mutex_lock(&mutex);
    release_waiter = 1;
    pthread_cond_broadcast(&condition);
    pthread_mutex_unlock(&mutex);
    if (pthread_join(busy, NULL) != 0 || pthread_join(waiting, NULL) != 0) return 5;
    if (pthread_create(&fresh, NULL, new_worker, NULL) != 0 || pthread_join(fresh, NULL) != 0) return 6;
    if (atomic_load(&failed) || tids[0] <= 1 || tids[1] <= 1 || tids[0] == tids[1]) return 7;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2 || prepare_output(argv[1]) != 0) return 2;
    process_ready = mmap(NULL, sizeof *process_ready, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (process_ready == MAP_FAILED) return 2;
    atomic_init(process_ready, 0);
    pid_t child = fork();
    if (child < 0) return 2;
    if (child == 0) {
        int result = run_process(argv[1], 0);
        if (result == 0) dprintf(STDOUT_FILENO, "CHILD-THREADS-RESTORED\n");
        return result;
    }
    int result = run_process(argv[1], 1);
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 8;
    if (result != 0) return result;
    dprintf(STDOUT_FILENO, "THREADS-RESTORED\n");
    return 0;
}
