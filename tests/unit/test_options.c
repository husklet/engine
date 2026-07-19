#define _POSIX_C_SOURCE 200809L

#include "test.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "../../src/core/options.h"

typedef struct option_thread {
    hl_options *options;
    const char *expected;
    int ok;
} option_thread;

static void *check_bound_option(void *opaque) {
    option_thread *thread = opaque;
    hl_options *previous = hl_options_bind(thread->options);
    thread->ok = strcmp(hl_option_get("HL_CWD"), thread->expected) == 0;
    (void)hl_options_bind(previous);
    return NULL;
}

int main(void) {
    char mutable[] = "original";
    hl_options first, second, snapshot;
    option_thread first_thread, second_thread;
    pthread_t first_id, second_id;

    hl_option_reset();
    HL_CHECK(setenv("HL_CWD", "/ambient", 1) == 0);
    HL_CHECK(setenv("MAPDUMP", "/tmp/legacy-mapdump", 1) == 0);
    HL_CHECK(hl_option_get("HL_CWD") == NULL);

    HL_CHECK(hl_options_init(&first) == 0);
    HL_CHECK(hl_options_init(&second) == 0);
    HL_CHECK(hl_options_set(&first, "HL_CWD", "/first", 1) == 0);
    HL_CHECK(hl_options_set(&second, "HL_CWD", "/second", 1) == 0);
    HL_CHECK(hl_options_set(&first, "HL_NOT_REGISTERED", "bad", 1) == -1);
    HL_CHECK(strcmp(hl_options_get(&first, "HL_CWD"), "/first") == 0);
    HL_CHECK(strcmp(hl_options_get(&first, "HL_CWD"), "/first") == 0);
    HL_CHECK(strcmp(hl_options_get(&second, "HL_CWD"), "/second") == 0);
    {
        hl_options *previous = hl_options_bind(&first);
        HL_CHECK(hl_options_clone_current(&snapshot) == 0);
        (void)hl_options_bind(previous);
    }
    HL_CHECK(hl_options_set(&first, "HL_CWD", "/mutated", 1) == 0);
    HL_CHECK(strcmp(hl_options_get(&snapshot, "HL_CWD"), "/first") == 0);
    hl_options_destroy(&snapshot);
    HL_CHECK(hl_options_set(&first, "HL_CWD", "/first", 1) == 0);
    first_thread = (option_thread){&first, "/first", 0};
    second_thread = (option_thread){&second, "/second", 0};
    HL_CHECK(pthread_create(&first_id, NULL, check_bound_option, &first_thread) == 0);
    HL_CHECK(pthread_create(&second_id, NULL, check_bound_option, &second_thread) == 0);
    HL_CHECK(pthread_join(first_id, NULL) == 0);
    HL_CHECK(pthread_join(second_id, NULL) == 0);
    HL_CHECK(first_thread.ok && second_thread.ok);
    /* Scoped stores never mutate the caller's default store. */
    HL_CHECK(hl_option_get("HL_CWD") == NULL);
    hl_options_destroy(&first);
    hl_options_destroy(&second);
    HL_CHECK(hl_option_get("MAPDUMP") == NULL);
    HL_CHECK(hl_option_set("MAPDUMP", "value", 1) == -1);
    HL_CHECK(hl_option_set("HL_CWD", mutable, 1) == 0);
    mutable[0] = 'X';
    HL_CHECK(strcmp(hl_option_get("HL_CWD"), "original") == 0);
    HL_CHECK(hl_option_set("HL_CWD", "ignored", 0) == 0);
    HL_CHECK(strcmp(hl_option_get("HL_CWD"), "original") == 0);
    HL_CHECK(hl_option_set("HL_CWD", "replacement", 1) == 0);
    HL_CHECK(strcmp(hl_option_get("HL_CWD"), "replacement") == 0);
    HL_CHECK(hl_option_unset("HL_CWD") == 0 && hl_option_get("HL_CWD") == NULL);
    HL_CHECK(hl_option_set("HL_NOT_REGISTERED", "value", 1) == -1);
    HL_CHECK(hl_option_unset("HL_NOT_REGISTERED") == -1);
    hl_option_reset();
    HL_CHECK(hl_option_get("HL_CWD") == NULL);
    unsetenv("HL_CWD");
    unsetenv("MAPDUMP");
    return EXIT_SUCCESS;
}
