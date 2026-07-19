#define _POSIX_C_SOURCE 200809L
#include "../../src/linux_abi/dns.h"
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static atomic_int running = 1;

static void resolve_localhost(void) {
    struct addrinfo hints = {0};
    struct addrinfo *result = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("localhost", NULL, &hints, &result) != 0) _exit(20);
    freeaddrinfo(result);
}

static void *resolver_thread(void *unused) {
    (void)unused;
    while (atomic_load_explicit(&running, memory_order_relaxed)) resolve_localhost();
    return NULL;
}

int main(void) {
    pthread_t worker;
    hl_linux_dns_prepare();
    if (pthread_create(&worker, NULL, resolver_thread, NULL) != 0) return 1;
    for (int attempt = 0; attempt < 200; attempt++) {
        pid_t child = fork();
        if (child < 0) return 2;
        if (child == 0) {
            resolve_localhost();
            _exit(0);
        }
        int status = 0;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 3;
    }
    atomic_store_explicit(&running, 0, memory_order_relaxed);
    return pthread_join(worker, NULL) == 0 ? 0 : 4;
}
