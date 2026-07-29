#include <unistd.h>

int main(void) {
    pid_t child = fork();
    if (child < 0) return 2;
    if (child == 0) {
        if (setsid() < 0) _exit(3);
        if (write(STDOUT_FILENO, "R", 1) != 1) _exit(4);
    }
    for (;;)
        pause();
}
