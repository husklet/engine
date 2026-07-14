#define _POSIX_C_SOURCE 200809L
#include "test.h"

#include "../../src/host/child.h"

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    hl_host_child_watch watch = {.read_descriptor = -1, .write_descriptor = -1};
    HL_CHECK(hl_host_child_watch_init(&watch) == 0);
    HL_CHECK(hl_host_child_watch_descriptor(&watch) >= 0);
    pid_t child = fork();
    HL_CHECK(child >= 0);
    if (child == 0) _exit(37);
    struct pollfd event = {.fd = hl_host_child_watch_descriptor(&watch), .events = POLLIN};
    int ready;
    do {
        ready = poll(&event, 1, 5000);
    } while (ready < 0);
    HL_CHECK(ready == 1 && (event.revents & POLLIN) != 0);
    hl_host_child_watch_drain(&watch);
    int status = 0;
    HL_CHECK(waitpid(child, &status, 0) == child);
    HL_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 37);
    hl_host_child_watch_notify(&watch);
    event.revents = 0;
    HL_CHECK(poll(&event, 1, 0) == 1 && (event.revents & POLLIN) != 0);
    hl_host_child_watch_close(&watch);
    HL_CHECK(hl_host_child_watch_descriptor(&watch) == -1);
    return 0;
}
