#include <signal.h>
#include <unistd.h>

static void on_term(int signal_number) {
    static const char message[] = "GOT_TERM";
    (void)signal_number;
    if (write(STDOUT_FILENO, message, sizeof(message) - 1) < 0) _exit(2);
    _exit(0);
}

int main(void) {
    static const char ready[] = "READY";
    if (signal(SIGTERM, on_term) == SIG_ERR) return 2;
    if (write(STDOUT_FILENO, ready, sizeof(ready) - 1) < 0) return 3;
    for (;;) {}
}
