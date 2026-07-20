// Controlling-process (session leader) death sends SIGHUP+SIGCONT to the controlling terminal's
// FOREGROUND process group (the classic "close the terminal -> children get SIGHUP"). Deterministic
// via a grandparent pipe: the foreground worker reports the signals it received after the leader dies.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

static char name[128];
static volatile sig_atomic_t got_hup, got_cont;
static void on_hup(int s){ (void)s; got_hup=1; }
static void on_cont(int s){ (void)s; got_cont=1; }

int main(void) {
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    grantpt(m); unlockpt(m);
    char *sn = ptsname(m);
    strncpy(name, sn ? sn : "", sizeof name - 1);

    int out[2]; if (pipe(out) < 0) return 1;
    int rdy[2]; if (pipe(rdy) < 0) return 1;

    pid_t c = fork();
    if (c == 0) {
        close(out[0]); close(rdy[0]);
        setsid();
        int s = open(name, O_RDWR);   // acquire ctty (session leader, no O_NOCTTY)
        ioctl(s, TIOCSCTTY, 0);
        pid_t w = fork();
        if (w == 0) {
            close(rdy[1]);
            setpgid(0, 0);
            struct sigaction sa; memset(&sa,0,sizeof sa);
            sa.sa_handler = on_hup;  sigaction(SIGHUP, &sa, 0);
            sa.sa_handler = on_cont; sigaction(SIGCONT, &sa, 0);
            char r; read(rdy[0], &r, 1);   // wait until we are the foreground group
            for (int i=0;i<3000 && !got_hup;i++) usleep(1000);
            unsigned char res = (got_hup?1:0)|(got_cont?2:0);
            write(out[1], &res, 1);
            _exit(0);
        }
        setpgid(w, w);
        tcsetpgrp(s, w);                   // W is foreground
        char go='G'; write(rdy[1], &go, 1);// release W now that it is foreground
        usleep(30000);
        _exit(0);                          // controlling-process death -> SIGHUP+SIGCONT to W
    }
    close(out[1]); close(rdy[0]); close(rdy[1]);
    unsigned char res = 0xff;
    read(out[0], &res, 1);
    int st; waitpid(c, &st, 0);
    printf("slhup fg_sighup=%d fg_sigcont=%d\n", (res&1)!=0, (res&2)!=0);
    close(m);
    return 0;
}
