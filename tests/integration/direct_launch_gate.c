#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition)                                                                                              \
    do {                                                                                                              \
        if (!(condition)) {                                                                                           \
            fprintf(stderr, "direct-launch test failed at line %d: %s\n", __LINE__, #condition);                    \
            return 1;                                                                                                 \
        }                                                                                                             \
    } while (0)

static int write_all(int descriptor, const void *data, size_t size) {
    const unsigned char *bytes = data;
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = write(descriptor, bytes + offset, size - offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (count == 0) return -1;
        offset += (size_t)count;
    }
    return 0;
}

static int fake_runner(int argc, char **argv) {
    char working_directory[4096];
    char record[8192];
    struct stat metadata;
    char payload[64] = {0};
    const char *separator;
    int input;
    int output;
    long exit_code;

    if (argc != 5 || strcmp(argv[1], "env") != 0) return 91;
    separator = strchr(argv[2], ':');
    if (separator == NULL) return 92;
    exit_code = strtol(argv[2], NULL, 10);
    const char *record_path = separator + 1;
    input = open(argv[3], O_RDONLY);
    if (input < 0) return 93;
    ssize_t payload_size = read(input, payload, sizeof payload);
    close(input);
    if (payload_size != 18 || memcmp(payload, "direct-launch-data", 18) != 0 || stat(argv[3], &metadata) != 0 ||
        (metadata.st_mode & 0777) != 0700 || getcwd(working_directory, sizeof working_directory) == NULL ||
        strcmp(strrchr(argv[3], '/') + 1, "guest") != 0 || strcmp(argv[4], "42") != 0)
        return 94;
    int length = snprintf(record, sizeof record, "%s\n%ld\n", working_directory, (long)getpid());
    if (length < 0 || (size_t)length >= sizeof record) return 95;
    output = open(record_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (output < 0 || write_all(output, record, (size_t)length) != 0 || close(output) != 0) return 96;
    if (exit_code == 99) {
        for (;;)
            pause();
    }
    return (int)exit_code;
}

static int run_gate(const char *gate, const char *self, const char *mode, const char *engine, const char *guest,
                    const char *temporary, int unset_temporary) {
    pid_t child = fork();
    int status;
    if (child < 0) return -1;
    if (child == 0) {
        if (unset_temporary)
            unsetenv("TMPDIR");
        else
            setenv("TMPDIR", temporary, 1);
        execl(gate, gate, mode, self, engine, guest, "42", (char *)NULL);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

static int run_bad_arity(const char *gate) {
    pid_t child = fork();
    int status;
    if (child < 0) return -1;
    if (child == 0) {
        execl(gate, gate, "cli", (char *)NULL);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int no_gate_workspace(const char *path) {
    DIR *directory = opendir(path);
    struct dirent *entry;
    if (directory == NULL) return 0;
    while ((entry = readdir(directory)) != NULL) {
        if (strncmp(entry->d_name, "hl-direct-launch.", strlen("hl-direct-launch.")) == 0) {
            closedir(directory);
            return 0;
        }
    }
    closedir(directory);
    return 1;
}

static int read_record(const char *path, char *workspace, size_t workspace_size, pid_t *child) {
    char buffer[8192];
    int descriptor = open(path, O_RDONLY);
    if (descriptor < 0) return -1;
    ssize_t count = read(descriptor, buffer, sizeof buffer - 1);
    close(descriptor);
    if (count <= 0) return -1;
    buffer[count] = '\0';
    char *newline = strchr(buffer, '\n');
    if (newline == NULL) return -1;
    *newline = '\0';
    if (snprintf(workspace, workspace_size, "%s", buffer) >= (int)workspace_size) return -1;
    *child = (pid_t)strtol(newline + 1, NULL, 10);
    return 0;
}

static int wait_for_record(const char *path) {
    const struct timespec tick = {0, 10000000};
    for (int attempt = 0; attempt < 500; ++attempt) {
        if (access(path, F_OK) == 0) return 0;
        nanosleep(&tick, NULL);
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc == 5 && strcmp(argv[1], "env") == 0) return fake_runner(argc, argv);
    CHECK(argc == 2);
    char root_template[] = "/tmp/hl-direct-launch-test.XXXXXX";
    char root[4096];
    CHECK(mkdtemp(root_template) != NULL);
    CHECK(realpath(root_template, root) != NULL);
    char guest[4096], record[4096], engine[8192], workspace[4096], fallback_root[4096];
    CHECK(realpath("/tmp", fallback_root) != NULL);
    CHECK(snprintf(guest, sizeof guest, "%s/source", root) < (int)sizeof guest);
    int descriptor = open(guest, O_WRONLY | O_CREAT | O_EXCL, 0644);
    CHECK(descriptor >= 0);
    CHECK(write_all(descriptor, "direct-launch-data", 18) == 0);
    CHECK(close(descriptor) == 0);

    CHECK(snprintf(record, sizeof record, "%s/success", root) < (int)sizeof record);
    CHECK(snprintf(engine, sizeof engine, "0:%s", record) < (int)sizeof engine);
    CHECK(run_gate(argv[1], argv[0], "cli", engine, guest, root, 0) == 0);
    pid_t runner_pid;
    CHECK(read_record(record, workspace, sizeof workspace, &runner_pid) == 0);
    CHECK(strncmp(workspace, root, strlen(root)) == 0);
    CHECK(access(workspace, F_OK) != 0 && errno == ENOENT);
    CHECK(unlink(record) == 0);
    CHECK(no_gate_workspace(root));

    CHECK(snprintf(record, sizeof record, "%s/status", root) < (int)sizeof record);
    CHECK(snprintf(engine, sizeof engine, "23:%s", record) < (int)sizeof engine);
    CHECK(run_gate(argv[1], argv[0], "config", engine, guest, root, 0) == 23);
    CHECK(read_record(record, workspace, sizeof workspace, &runner_pid) == 0);
    CHECK(access(workspace, F_OK) != 0 && errno == ENOENT);
    CHECK(unlink(record) == 0);

    CHECK(snprintf(record, sizeof record, "%s/fallback-empty", root) < (int)sizeof record);
    CHECK(snprintf(engine, sizeof engine, "0:%s", record) < (int)sizeof engine);
    CHECK(run_gate(argv[1], argv[0], "cli", engine, guest, "", 0) == 0);
    CHECK(read_record(record, workspace, sizeof workspace, &runner_pid) == 0);
    CHECK(strncmp(workspace, fallback_root, strlen(fallback_root)) == 0);
    CHECK(strstr(workspace + strlen(fallback_root), "/hl-direct-launch.") ==
          workspace + strlen(fallback_root));
    CHECK(access(workspace, F_OK) != 0 && errno == ENOENT);
    CHECK(unlink(record) == 0);

    CHECK(run_bad_arity(argv[1]) == 2);
    CHECK(run_gate(argv[1], argv[0], "invalid", engine, guest, root, 0) == 2);
    CHECK(run_gate(argv[1], argv[0], "cli", engine, "/missing/direct-launch-guest", root, 0) == 1);
    CHECK(run_gate(argv[1], "/missing/direct-launch-runner", "cli", engine, guest, root, 0) == 127);
    CHECK(no_gate_workspace(root));

    CHECK(snprintf(record, sizeof record, "%s/fallback", root) < (int)sizeof record);
    CHECK(snprintf(engine, sizeof engine, "0:%s", record) < (int)sizeof engine);
    CHECK(run_gate(argv[1], argv[0], "cli", engine, guest, root, 1) == 0);
    CHECK(read_record(record, workspace, sizeof workspace, &runner_pid) == 0);
    CHECK(strncmp(workspace, fallback_root, strlen(fallback_root)) == 0);
    CHECK(strstr(workspace + strlen(fallback_root), "/hl-direct-launch.") ==
          workspace + strlen(fallback_root));
    CHECK(access(workspace, F_OK) != 0 && errno == ENOENT);
    CHECK(unlink(record) == 0);

    const int signals[] = {SIGHUP, SIGINT, SIGTERM};
    for (size_t index = 0; index < sizeof signals / sizeof signals[0]; ++index) {
        CHECK(snprintf(record, sizeof record, "%s/signal-%d", root, signals[index]) < (int)sizeof record);
        CHECK(snprintf(engine, sizeof engine, "99:%s", record) < (int)sizeof engine);
        pid_t gate = fork();
        CHECK(gate >= 0);
        if (gate == 0) {
            setenv("TMPDIR", root, 1);
            execl(argv[1], argv[1], "cli", argv[0], engine, guest, "42", (char *)NULL);
            _exit(127);
        }
        CHECK(wait_for_record(record) == 0);
        CHECK(read_record(record, workspace, sizeof workspace, &runner_pid) == 0);
        CHECK(kill(gate, signals[index]) == 0);
        int status;
        CHECK(waitpid(gate, &status, 0) == gate);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 128 + signals[index]);
        CHECK(kill(runner_pid, 0) != 0 && errno == ESRCH);
        CHECK(access(workspace, F_OK) != 0 && errno == ENOENT);
        CHECK(unlink(record) == 0);
    }

    CHECK(unlink(guest) == 0);
    CHECK(rmdir(root) == 0);
    return 0;
}
