#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum domain { DOMAIN_TRANSLATOR, DOMAIN_LINUX_ABI, DOMAIN_TARGET, DOMAIN_COUNT, DOMAIN_NONE };

enum category {
    CAT_PLATFORM,
    CAT_FILESYSTEM,
    CAT_MEMORY,
    CAT_PROCESS,
    CAT_THREAD,
    CAT_NETWORK,
    CAT_SIGNAL,
    CAT_TIME,
    CAT_SYSTEM,
    CAT_COUNT
};

static const char *const domain_names[] = {"translator", "linux-abi", "target"};
static const char *const category_names[] = {"platform-header", "filesystem", "memory", "process", "thread",
                                             "network",         "signal",     "time",   "system"};

/* The checked inventory describes the production tree, not an intended allowance. */
static const unsigned long baseline[DOMAIN_COUNT][CAT_COUNT] = {
    {1, 1, 2, 4, 29, 0, 1, 8, 0},
    {0, 824, 70, 181, 358, 100, 39, 19, 14},
    {3, 12, 4, 6, 1, 0, 4, 0, 0},
};

static const char *const filesystem_calls[] = {
    "access",    "chdir",     "chmod",    "chown",      "close",     "closedir", "dup",      "dup2",    "dup3",
    "faccessat", "fchmod",    "fchown",   "fcntl",      "fdopendir", "flock",    "fstat",    "fstatat", "fsync",
    "ftruncate", "getcwd",    "ioctl",    "link",       "linkat",    "lseek",    "lstat",    "mkdir",   "mkdirat",
    "open",      "openat",    "opendir",  "pipe",       "pipe2",     "poll",     "ppoll",    "pread",   "pwrite",
    "read",      "readdir",   "readlink", "readlinkat", "realpath",  "rename",   "renameat", "rmdir",   "stat",
    "symlink",   "symlinkat", "truncate", "unlink",     "unlinkat",  "write"};
static const char *const memory_calls[] = {"madvise", "mlock", "mmap",   "mprotect", "msync", "munlock",
                                           "munmap",  "shmat", "shmctl", "shmdt",    "shmget"};
static const char *const process_calls[] = {"_exit",  "execv",   "execve", "execvp", "fork",        "getpgid",
                                            "getpid", "getppid", "getsid", "kill",   "posix_spawn", "setpgid",
                                            "setsid", "vfork",   "wait",   "waitid", "waitpid"};
static const char *const thread_calls[] = {
    "pthread_atfork",         "pthread_cond_broadcast", "pthread_cond_destroy",  "pthread_cond_init",
    "pthread_cond_signal",    "pthread_cond_wait",      "pthread_create",        "pthread_detach",
    "pthread_join",           "pthread_kill",           "pthread_mutex_destroy", "pthread_mutex_init",
    "pthread_mutex_lock",     "pthread_mutex_trylock",  "pthread_mutex_unlock",  "pthread_once",
    "pthread_rwlock_destroy", "pthread_rwlock_init",    "pthread_rwlock_rdlock", "pthread_rwlock_unlock",
    "pthread_rwlock_wrlock",  "pthread_sigmask"};
static const char *const network_calls[] = {
    "accept",   "accept4", "bind", "connect", "getpeername", "getsockname", "getsockopt", "listen", "recv",
    "recvfrom", "recvmsg", "send", "sendmsg", "sendto",      "setsockopt",  "shutdown",   "socket", "socketpair"};
static const char *const signal_calls[] = {"raise",       "sigaction",  "sigaltstack",  "sigpending",
                                           "sigprocmask", "sigsuspend", "sigtimedwait", "sigwait"};
static const char *const time_calls[] = {"clock_getres", "clock_gettime", "clock_nanosleep",
                                         "gettimeofday", "nanosleep",     "setitimer",
                                         "timer_create", "timer_delete",  "timer_settime"};
static const char *const system_calls[] = {"getenv",    "getrlimit", "getrusage", "prctl", "setenv",
                                           "setrlimit", "syscall",   "sysconf",   "uname", "unsetenv"};

static enum domain path_domain(const char *path) {
    if (strstr(path, "src/translator/") != NULL) return DOMAIN_TRANSLATOR;
    if (strstr(path, "src/linux_abi/") != NULL) return DOMAIN_LINUX_ABI;
    if (strstr(path, "src/core/target/") != NULL) return DOMAIN_TARGET;
    return DOMAIN_NONE;
}

static int listed(const char *word, const char *const *items, size_t count) {
    size_t i;
    for (i = 0; i < count; ++i)
        if (strcmp(word, items[i]) == 0) return 1;
    return 0;
}

static enum category call_category(const char *word) {
#define MEMBER(a) listed(word, (a), sizeof(a) / sizeof((a)[0]))
    if (MEMBER(filesystem_calls)) return CAT_FILESYSTEM;
    if (MEMBER(memory_calls)) return CAT_MEMORY;
    if (MEMBER(process_calls)) return CAT_PROCESS;
    if (MEMBER(thread_calls)) return CAT_THREAD;
    if (MEMBER(network_calls)) return CAT_NETWORK;
    if (MEMBER(signal_calls)) return CAT_SIGNAL;
    if (MEMBER(time_calls)) return CAT_TIME;
    if (MEMBER(system_calls)) return CAT_SYSTEM;
#undef MEMBER
    return CAT_COUNT;
}

static int platform_header(const char *header) {
    return strncmp(header, "mach/", 5) == 0 || strncmp(header, "libkern/", 8) == 0 ||
           strncmp(header, "linux/", 6) == 0 || strcmp(header, "sys/event.h") == 0 || strcmp(header, "windows.h") == 0;
}

static char *read_file(const char *path, size_t *size_out) {
    FILE *file = fopen(path, "rb");
    long length;
    char *data;
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = malloc((size_t)length + 1);
    if (data == NULL || fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    data[length] = '\0';
    *size_out = (size_t)length;
    fclose(file);
    return data;
}

static void scan_buffer(const char *path, const char *data, size_t size, enum domain domain,
                        unsigned long counts[DOMAIN_COUNT][CAT_COUNT], int verbose) {
    size_t i = 0;
    unsigned long line = 1;
    unsigned int brace_depth = 0;
    while (i < size) {
        if (data[i] == '\n') {
            ++line;
            ++i;
            continue;
        }
        if (data[i] == '/' && i + 1 < size && data[i + 1] == '/') {
            i += 2;
            while (i < size && data[i] != '\n')
                ++i;
            continue;
        }
        if (data[i] == '/' && i + 1 < size && data[i + 1] == '*') {
            i += 2;
            while (i + 1 < size && !(data[i] == '*' && data[i + 1] == '/')) {
                if (data[i] == '\n') ++line;
                ++i;
            }
            if (i + 1 < size) i += 2;
            continue;
        }
        if (data[i] == '#') {
            size_t p = i + 1, begin, end;
            while (p < size && (data[p] == ' ' || data[p] == '\t'))
                ++p;
            if (p + 7 <= size && strncmp(data + p, "include", 7) == 0 && !isalnum((unsigned char)data[p + 7])) {
                p += 7;
                while (p < size && isspace((unsigned char)data[p]) && data[p] != '\n')
                    ++p;
                if (p < size && (data[p] == '<' || data[p] == '"')) {
                    char close = data[p++] == '<' ? '>' : '"';
                    begin = p;
                    while (p < size && data[p] != close && data[p] != '\n')
                        ++p;
                    end = p;
                    if (end - begin < 256) {
                        char header[256];
                        memcpy(header, data + begin, end - begin);
                        header[end - begin] = '\0';
                        if (platform_header(header)) {
                            ++counts[domain][CAT_PLATFORM];
                            if (verbose)
                                fprintf(stderr, "%s:%lu: %s bypass: platform header <%s>\n", path, line,
                                        domain_names[domain], header);
                        }
                    }
                }
            }
            if (p + 7 <= size && strncmp(data + p, "include", 7) == 0 && !isalnum((unsigned char)data[p + 7])) {
                while (i < size && data[i] != '\n')
                    ++i;
                continue;
            }
            ++i;
            continue;
        }
        if (data[i] == '"' || data[i] == '\'') {
            char quote = data[i++];
            while (i < size && data[i] != quote) {
                if (data[i] == '\\' && i + 1 < size)
                    i += 2;
                else {
                    if (data[i] == '\n') ++line;
                    ++i;
                }
            }
            if (i < size) ++i;
            continue;
        }
        if (isalpha((unsigned char)data[i]) || data[i] == '_') {
            size_t begin = i, p;
            char word[128];
            enum category category;
            while (i < size && (isalnum((unsigned char)data[i]) || data[i] == '_'))
                ++i;
            p = i;
            while (p < size && isspace((unsigned char)data[p]))
                ++p;
            if (brace_depth != 0 && p < size && data[p] == '(' && i - begin < sizeof(word)) {
                size_t before = begin;
                while (before > 0 && (data[before - 1] == ' ' || data[before - 1] == '\t'))
                    --before;
                /* service.read() and service->read() are dependency injection, not ambient calls. */
                if (before > 0 &&
                    (data[before - 1] == '.' || (data[before - 1] == '>' && before > 1 && data[before - 2] == '-'))) {
                    continue;
                }
                memcpy(word, data + begin, i - begin);
                word[i - begin] = '\0';
                category = call_category(word);
                if (category != CAT_COUNT) {
                    ++counts[domain][category];
                    if (verbose)
                        fprintf(stderr, "%s:%lu: %s bypass: %s call %s()\n", path, line, domain_names[domain],
                                category_names[category], word);
                }
            }
            continue;
        }
        if (data[i] == '{') ++brace_depth;
        if (data[i] == '}' && brace_depth != 0) --brace_depth;
        ++i;
    }
}

static void scan(const char *path, enum domain domain, unsigned long counts[DOMAIN_COUNT][CAT_COUNT], int verbose) {
    size_t size;
    char *data = read_file(path, &size);
    if (data == NULL) {
        perror(path);
        exit(2);
    }
    scan_buffer(path, data, size, domain, counts, verbose);
    free(data);
}

static int inventory_matches(const unsigned long *counts) {
    enum domain d;
    enum category c;
    for (d = 0; d < DOMAIN_COUNT; ++d)
        for (c = 0; c < CAT_COUNT; ++c)
            if (counts[(size_t)d * CAT_COUNT + (size_t)c] != baseline[d][c]) return 0;
    return 1;
}

static int self_test(void) {
    static const char source[] = "// open(); #include <mach/comment.h>\n"
                                 "const char *s = \"read(); #include <linux/string.h>\";\n"
                                 "static int read(int fd) { return fd; }\n"
                                 "static void test(void) { svc.open(); svc->read(); open(\"x\", 0); }\n"
                                 "#include <mach/real.h>\n";
    unsigned long counts[DOMAIN_COUNT][CAT_COUNT] = {{0}};
    unsigned long changed[DOMAIN_COUNT][CAT_COUNT];
    scan_buffer("src/translator/selftest.c", source, sizeof(source) - 1, DOMAIN_TRANSLATOR, counts, 0);
    if (counts[DOMAIN_TRANSLATOR][CAT_PLATFORM] != 1 || counts[DOMAIN_TRANSLATOR][CAT_FILESYSTEM] != 1) return 1;
    memcpy(changed, baseline, sizeof(changed));
    if (!inventory_matches(&changed[0][0])) return 1;
    ++changed[DOMAIN_TRANSLATOR][CAT_FILESYSTEM];
    if (inventory_matches(&changed[0][0])) return 1;
    changed[DOMAIN_TRANSLATOR][CAT_FILESYSTEM] -= 2;
    if (inventory_matches(&changed[0][0])) return 1;
    return 0;
}

int main(int argc, char **argv) {
    unsigned long counts[DOMAIN_COUNT][CAT_COUNT] = {{0}};
    int measure = argc > 1 && strcmp(argv[1], "--measure") == 0;
    int first = measure ? 2 : 1, i, failed = 0;
    enum domain d;
    enum category c;
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0) return self_test() ? EXIT_FAILURE : EXIT_SUCCESS;
    if (argc <= first) {
        fprintf(stderr, "usage: check-domains [--measure] FILE...\n");
        return 2;
    }
    for (i = first; i < argc; ++i) {
        d = path_domain(argv[i]);
        if (d != DOMAIN_NONE) scan(argv[i], d, counts, measure);
    }
    for (d = 0; d < DOMAIN_COUNT; ++d)
        for (c = 0; c < CAT_COUNT; ++c) {
            if (counts[d][c] != 0 || baseline[d][c] != 0)
                fprintf(stderr, "domain inventory: %-10s %-15s current=%lu baseline=%lu\n", domain_names[d],
                        category_names[c], counts[d][c], baseline[d][c]);
            if (!measure && counts[d][c] != baseline[d][c]) {
                fprintf(stderr, "domain inventory mismatch: %s %s %s by %lu; %s the checked baseline\n",
                        domain_names[d], category_names[c], counts[d][c] > baseline[d][c] ? "grew" : "shrunk",
                        counts[d][c] > baseline[d][c] ? counts[d][c] - baseline[d][c] : baseline[d][c] - counts[d][c],
                        counts[d][c] > baseline[d][c] ? "remove the bypass or explicitly review" : "shrink");
                failed = 1;
            }
        }
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
