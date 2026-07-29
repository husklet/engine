// A descriptor may belong to multiple independent epoll instances. A fork
// child inherits every interest list and its distinct user data.
#include <stdint.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <unistd.h>

static int wait_for(int epoll, uint64_t data) {
    struct epoll_event event = {0};
    int count = epoll_wait(epoll, &event, 1, 1000);
    return count == 1 && event.data.u64 == data && (event.events & EPOLLIN) != 0;
}

int main(void) {
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) return 1;

    int first = epoll_create1(0);
    int second = epoll_create1(0);
    if (first < 0 || second < 0) return 2;

    struct epoll_event first_event = {
        .events = EPOLLIN,
        .data.u64 = UINT64_C(0x1111222233334444),
    };
    struct epoll_event second_event = {
        .events = EPOLLIN,
        .data.u64 = UINT64_C(0xaaaabbbbccccdddd),
    };
    if (epoll_ctl(first, EPOLL_CTL_ADD, pipe_fd[0], &first_event) != 0 ||
        epoll_ctl(second, EPOLL_CTL_ADD, pipe_fd[0], &second_event) != 0)
        return 3;

    pid_t child = fork();
    if (child == 0) {
        if (write(pipe_fd[1], "x", 1) != 1) _exit(4);
        int first_ready = wait_for(first, first_event.data.u64);
        int second_ready = wait_for(second, second_event.data.u64);
        printf("epoll_fork_multiple first=%d second=%d\n", first_ready, second_ready);
        fflush(stdout);
        _exit(first_ready && second_ready ? 0 : 5);
    }

    int status = 0;
    waitpid(child, &status, 0);
    printf("epoll_fork_multiple child=%d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 6;
}
