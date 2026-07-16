#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    int identities[2];
    pid_t session;
    if (pipe(identities) != 0) return 2;
    session = fork();
    if (session < 0) return 3;
    if (session == 0) {
        pid_t daemon;
        close(identities[0]);
        if (setsid() < 0) _exit(4);
        daemon = fork();
        if (daemon < 0) _exit(5);
        if (daemon == 0) {
            close(identities[1]);
            for (;;) pause();
        }
        if (write(identities[1], &daemon, sizeof daemon) != sizeof daemon) _exit(6);
        close(identities[1]);
        _exit(0);
    }
    close(identities[1]);
    {
        pid_t daemon = 0;
        int status;
        ssize_t size;
        do { size = read(identities[0], &daemon, sizeof daemon); } while (size < 0 && errno == EINTR);
        close(identities[0]);
        if (size != sizeof daemon || daemon <= 0 || waitpid(session, &status, 0) != session ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            return 7;
        printf("%ld\n", (long)daemon);
        return fflush(stdout) == 0 ? 0 : 8;
    }
}
