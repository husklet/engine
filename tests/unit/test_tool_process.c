#include "../../tools/process.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void test_prefix(void) {
    char **argv = NULL;
    size_t argc = 0;

    assert(hl_process_split_prefix("mac docker", &argv, &argc) == 0);
    assert(argc == 2);
    assert(strcmp(argv[0], "mac") == 0);
    assert(strcmp(argv[1], "docker") == 0);
    hl_process_free_argv(argv);

    assert(hl_process_split_prefix("launcher --name 'two words'", &argv, &argc) == 0);
    assert(argc == 3);
    assert(strcmp(argv[2], "two words") == 0);
    hl_process_free_argv(argv);

    assert(hl_process_split_prefix("'unterminated", &argv, &argc) == -1);
    assert(errno == EINVAL);
}

static void test_capture(void) {
    char *argv[] = {"/bin/echo", "two words", NULL};
    char *output = NULL;
    size_t output_size = 0;
    hl_process_result_t result;

    assert(hl_process_run(argv, NULL, 0, &output, &output_size, &result) == 0);
    assert(result.exec_errno == 0);
    assert(result.exited);
    assert(result.exit_code == 0);
    assert(output_size == strlen("two words\n"));
    assert(strcmp(output, "two words\n") == 0);
    free(output);
}

static void test_exec_error(void) {
    char *argv[] = {"/hl/does/not/exist", NULL};
    hl_process_result_t result;

    assert(hl_process_run(argv, NULL, 1, NULL, NULL, &result) == 0);
    assert(result.exec_errno == ENOENT);
    assert(result.exited);
    assert(result.exit_code == 127);
}

int main(void) {
    test_prefix();
    test_capture();
    test_exec_error();
    return 0;
}
