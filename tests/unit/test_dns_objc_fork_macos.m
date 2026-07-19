#define _POSIX_C_SOURCE 200809L
#include "../../src/linux_abi/dns.h"

#import <Foundation/Foundation.h>

#include <pthread.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int ready;
static int go;

static void use_string(void) {
    @autoreleasepool {
        NSString *value = [NSString stringWithUTF8String:"ports.ubuntu.com"];
        if (value.length != 16) _exit(20);
    }
}

static void *initialize_string(void *unused) {
    (void)unused;
    if (pthread_mutex_lock(&lock) != 0) _exit(21);
    ready = 1;
    if (pthread_cond_broadcast(&changed) != 0) _exit(22);
    while (!go)
        if (pthread_cond_wait(&changed, &lock) != 0) _exit(23);
    if (pthread_mutex_unlock(&lock) != 0) _exit(24);
    use_string();
    return NULL;
}

static int probe(void) {
    pthread_t worker;
    hl_linux_dns_prepare();
    if (pthread_create(&worker, NULL, initialize_string, NULL) != 0) return 1;
    if (pthread_mutex_lock(&lock) != 0) return 2;
    while (!ready)
        if (pthread_cond_wait(&changed, &lock) != 0) return 3;
    go = 1;
    if (pthread_cond_broadcast(&changed) != 0) return 4;
    if (pthread_mutex_unlock(&lock) != 0) return 5;
    pid_t child = fork();
    if (child < 0) return 6;
    if (child == 0) {
        use_string();
        _exit(0);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child) return 7;
    if (pthread_join(worker, NULL) != 0) return 8;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 9;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "probe") == 0) return probe();
    for (int attempt = 0; attempt < 200; attempt++) {
        pid_t child = 0;
        char *arguments[] = {argv[0], "probe", NULL};
        if (posix_spawn(&child, argv[0], NULL, NULL, arguments, environ) != 0) return 9;
        int status = 0;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 10;
    }
    return 0;
}
