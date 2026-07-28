#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int make_pipe(int pipe_fd[2]) {
    if (pipe(pipe_fd) != 0) return -1;
    if (fcntl(pipe_fd[0], F_SETFD, FD_CLOEXEC) == 0 && fcntl(pipe_fd[1], F_SETFD, FD_CLOEXEC) == 0) return 0;
    int saved_errno = errno;
    close(pipe_fd[0]);
    close(pipe_fd[1]);
    errno = saved_errno;
    return -1;
}

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} output_buffer_t;

typedef struct {
    int input;
    int output;
    int error;
    int quiet;
} child_io_t;

static int append_output(output_buffer_t *output, const char *data, size_t data_length) {
    if (output->length > SIZE_MAX - data_length - 1) return -1;
    size_t required = output->length + data_length + 1;
    if (required > output->capacity) {
        size_t next = output->capacity ? output->capacity : 4096;
        while (next < required) {
            if (next > SIZE_MAX / 2) {
                next = required;
                break;
            }
            next *= 2;
        }
        char *grown = realloc(output->data, next);
        if (!grown) return -1;
        output->data = grown;
        output->capacity = next;
    }
    for (size_t index = 0; index < data_length; index++)
        output->data[output->length + index] = data[index];
    output->length += data_length;
    output->data[output->length] = '\0';
    return 0;
}

static void close_if_open(int *fd) {
    if (*fd < 0) return;
    close(*fd);
    *fd = -1;
}

static void child_fail(int error_fd) {
    int saved_errno = errno;
    ssize_t written = write(error_fd, &saved_errno, sizeof(saved_errno));
    (void)written;
    _exit(127);
}

static void child_exec(char *const argv[], child_io_t io) {
    int null_fd = -1;
    if (io.input >= 0 && dup2(io.input, STDIN_FILENO) < 0) child_fail(io.error);
    if (io.quiet) {
        null_fd = open("/dev/null", O_WRONLY);
        if (null_fd < 0 || dup2(null_fd, STDOUT_FILENO) < 0 || dup2(null_fd, STDERR_FILENO) < 0) child_fail(io.error);
    } else if (dup2(io.output, STDOUT_FILENO) < 0) {
        child_fail(io.error);
    }
    if (io.input > STDERR_FILENO) close(io.input);
    if (io.output > STDERR_FILENO) close(io.output);
    if (null_fd > STDERR_FILENO) close(null_fd);
    execvp(argv[0], argv);
    child_fail(io.error);
}

static int read_output(int fd, output_buffer_t *output) {
    char buffer[16384];
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            if (append_output(output, buffer, (size_t)count) != 0) return -1;
            continue;
        }
        if (count == 0) break;
        if (errno != EINTR) return -1;
    }
    return append_output(output, "", 0);
}

int hl_process_run(char *const argv[], const char *stdin_path, int quiet, char **stdout_text, size_t *stdout_size,
                   hl_process_result_t *result) {
    int output_pipe[2] = {-1, -1};
    int error_pipe[2] = {-1, -1};
    int input_fd = -1;
    int status = 0;
    int failed = 0;
    output_buffer_t output = {0};

    if (!argv || !argv[0] || !result) {
        errno = EINVAL;
        return -1;
    }
    *result = (hl_process_result_t){0};
    if (stdout_text) *stdout_text = NULL;
    if (stdout_size) *stdout_size = 0;
    if ((!quiet && make_pipe(output_pipe) != 0) || make_pipe(error_pipe) != 0) goto system_error;
    if (stdin_path) {
        input_fd = open(stdin_path, O_RDONLY);
        if (input_fd < 0) goto system_error;
    }

    pid_t child = fork();
    if (child < 0) goto system_error;
    if (child == 0) {
        close(error_pipe[0]);
        if (output_pipe[0] >= 0) close(output_pipe[0]);
        child_io_t io = {input_fd, output_pipe[1], error_pipe[1], quiet};
        child_exec(argv, io);
    }

    close_if_open(&input_fd);
    close_if_open(&error_pipe[1]);
    close_if_open(&output_pipe[1]);
    if (!quiet && read_output(output_pipe[0], &output) != 0) failed = 1;
    close_if_open(&output_pipe[0]);

    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child) failed = 1;

    ssize_t error_count;
    do {
        error_count = read(error_pipe[0], &result->exec_errno, sizeof(result->exec_errno));
    } while (error_count < 0 && errno == EINTR);
    if (error_count < 0) failed = 1;
    close_if_open(&error_pipe[0]);

    if (waited == child && WIFEXITED(status)) {
        result->exited = 1;
        result->exit_code = WEXITSTATUS(status);
    } else if (waited == child && WIFSIGNALED(status)) {
        result->signaled = 1;
        result->signal = WTERMSIG(status);
    }
    if (stdout_text)
        *stdout_text = output.data;
    else
        free(output.data);
    if (stdout_size) *stdout_size = output.length;
    return failed ? -1 : 0;

system_error:
    close_if_open(&input_fd);
    close_if_open(&output_pipe[0]);
    close_if_open(&output_pipe[1]);
    close_if_open(&error_pipe[0]);
    close_if_open(&error_pipe[1]);
    return -1;
}

static int is_space(char value) {
    return value == ' ' || value == '\t' || value == '\n';
}

int hl_process_split_prefix(const char *text, char ***argv_out, size_t *argc_out) {
    size_t capacity = 4;
    size_t argc = 0;
    char **argv;
    const char *cursor;
    int failure_errno = EINVAL;

    if (!text || !argv_out || !argc_out) {
        errno = EINVAL;
        return -1;
    }
    argv = (char **)calloc(capacity, sizeof(*argv));
    if (!argv) return -1;
    cursor = text;
    while (*cursor) {
        while (is_space(*cursor))
            cursor++;
        if (!*cursor) break;
        size_t length = 0;
        char *word = malloc(strlen(cursor) + 1);
        char quote = '\0';
        if (!word) {
            failure_errno = ENOMEM;
            goto fail;
        }
        while (*cursor && (quote || !is_space(*cursor))) {
            if (!quote && (*cursor == '\'' || *cursor == '"')) {
                quote = *cursor++;
            } else if (quote == *cursor) {
                quote = '\0';
                cursor++;
            } else if (*cursor == '\\' && quote != '\'') {
                cursor++;
                if (!*cursor) {
                    free(word);
                    goto fail;
                }
                word[length++] = *cursor++;
            } else {
                word[length++] = *cursor++;
            }
        }
        if (quote) {
            free(word);
            goto fail;
        }
        word[length] = '\0';
        if (argc + 2 > capacity) {
            capacity *= 2;
            char **grown = (char **)realloc((void *)argv, capacity * sizeof(*argv));
            if (!grown) {
                free(word);
                failure_errno = ENOMEM;
                goto fail;
            }
            argv = grown;
        }
        argv[argc++] = word;
        argv[argc] = NULL;
    }
    if (argc == 0) goto fail;
    *argv_out = argv;
    *argc_out = argc;
    return 0;

fail:
    hl_process_free_argv(argv);
    errno = failure_errno;
    return -1;
}

void hl_process_free_argv(char **argv) {
    if (!argv) return;
    for (size_t index = 0; argv[index]; index++)
        free(argv[index]);
    free((void *)argv);
}
