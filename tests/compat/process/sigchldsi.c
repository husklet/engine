// DISCOVERY probe: SA_SIGINFO SIGCHLD siginfo vs waitid for exit/kill/stop/cont fates.
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile int g_code, g_status, g_pid, g_signo, g_n;
static void ch(int s, siginfo_t *si, void *u) {
    (void)s; (void)u;
    g_signo = si->si_signo; g_code = si->si_code; g_status = si->si_status; g_pid = si->si_pid; g_n++;
}

int main(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = ch; sa.sa_flags = SA_SIGINFO; // no SA_NOCLDSTOP: stop/cont generate SIGCHLD
    sigaction(SIGCHLD, &sa, NULL);

    // exit
    g_n = 0;
    pid_t p = fork();
    if (p == 0) _exit(7);
    while (g_n == 0) pause();
    printf("exit signo=%d code=%d(want%d) status=%d pid=%d\n", g_signo, g_code, CLD_EXITED, g_status, g_pid == p);
    int st; waitpid(p, &st, 0);

    // killed
    g_n = 0;
    pid_t k = fork();
    if (k == 0) { pause(); _exit(0); }
    usleep(30000); kill(k, SIGKILL);
    while (g_n == 0) pause();
    printf("kill code=%d(want%d) status=%d(want%d)\n", g_code, CLD_KILLED, g_status, SIGKILL);
    waitpid(k, &st, 0);

    // stopped (SA_NOCLDSTOP absent => SIGCHLD on stop)
    g_n = 0;
    pid_t s = fork();
    if (s == 0) { raise(SIGSTOP); _exit(3); }
    while (g_n == 0) pause();
    printf("stop code=%d(want%d) status=%d(want%d)\n", g_code, CLD_STOPPED, g_status, SIGSTOP);
    // continued
    g_n = 0;
    kill(s, SIGCONT);
    while (g_n == 0) pause();
    printf("cont code=%d(want%d) status=%d(want%d)\n", g_code, CLD_CONTINUED, g_status, SIGCONT);
    waitpid(s, &st, 0);
    printf("stop-child-exit code=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    printf("done\n");
    return 0;
}
