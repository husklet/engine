#define _POSIX_C_SOURCE 200809L
#include "test.h"

#include "../../src/host/process.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    int hold[2];
    HL_CHECK(pipe(hold) == 0);
    pid_t child = fork();
    HL_CHECK(child >= 0);
    if (child == 0) {
        close(hold[1]);
        char byte;
        ssize_t ignored = read(hold[0], &byte, 1);
        (void)ignored;
        _exit(29);
    }
    close(hold[0]);
    int descriptor = hl_host_process_open(child);
    HL_CHECK(descriptor >= 0);
    struct pollfd event = {.fd = descriptor, .events = POLLIN};
    HL_CHECK(poll(&event, 1, 0) == 0);
    close(hold[1]);
    int ready;
    do {
        ready = poll(&event, 1, 5000);
    } while (ready < 0 && errno == EINTR);
    HL_CHECK(ready == 1 && (event.revents & POLLIN) != 0);
    event.revents = 0;
    HL_CHECK(poll(&event, 1, 0) == 1 && (event.revents & POLLIN) != 0);
    int status;
    HL_CHECK(waitpid(child, &status, 0) == child);
    HL_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 29);
    close(descriptor);
    return 0;
}
