#ifndef HL_TOOLS_PROCESS_H
#define HL_TOOLS_PROCESS_H

#include <stddef.h>

typedef struct {
    int exec_errno;
    int exited;
    int exit_code;
    int signaled;
    int signal;
} hl_process_result_t;

/* Runs argv directly. Captured stdout is allocated for the caller. */
int hl_process_run(char *const argv[], const char *stdin_path, int quiet, char **stdout_text, size_t *stdout_size,
                   hl_process_result_t *result);

/* Parses quoting in a command prefix without expansion or shell execution. */
int hl_process_split_prefix(const char *text, char ***argv_out, size_t *argc_out);
void hl_process_free_argv(char **argv);

#endif
