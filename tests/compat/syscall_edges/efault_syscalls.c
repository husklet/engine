// syscall-compat regression: kernel-side bad-pointer validation. Raw syscalls handed a (void*)-1 user buffer
// must fail with EFAULT from the kernel, never fault the calling task. Each probe runs in its own child so a
// mis-translation that faults is observed deterministically (reported as sig=N) rather than killing the
// harness. Arch-neutral: per-probe errno (or signal) printed. Output is unbuffered so a crash cannot hide it.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define BAD ((long)-1)

// Runs `syscall(nr,...)` in a child; returns the errno on failure, 0 on success, or -sig if it was killed.
static int probe(long nr, long a, long b, long c, long d, long e) {
    pid_t p = fork();
    if (p == 0) {
        long r = syscall(nr, a, b, c, d, e);
        _exit(r == -1 ? (errno & 0x7f) : 0);
    }
    int st = 0;
    waitpid(p, &st, 0);
    if (WIFSIGNALED(st)) return -WTERMSIG(st);
    return WEXITSTATUS(st);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int fd = open("/dev/zero", O_RDONLY);
    printf("getrusage=%d\n", probe(SYS_getrusage, 0, BAD, 0, 0, 0));
    printf("newfstatat=%d\n", probe(SYS_newfstatat, fd, (long)"", BAD, 0x1000 /*AT_EMPTY_PATH*/, 0));
    printf("gettimeofday=%d\n", probe(SYS_gettimeofday, BAD, 0, 0, 0, 0));
    printf("times=%d\n", probe(SYS_times, BAD, 0, 0, 0, 0));
    printf("read=%d\n", probe(SYS_read, fd, BAD, 16, 0, 0));
    printf("pipe2=%d\n", probe(SYS_pipe2, BAD, 0, 0, 0, 0));
    // inotify_add_watch with a wild path pointer: the kernel returns EFAULT; a handler that hands the
    // pointer straight to its path resolver faults the engine and kills the guest with sig=11 instead.
    int ino = syscall(SYS_inotify_init1, 0);
    printf("inotify_add_watch=%d\n", probe(SYS_inotify_add_watch, ino, BAD, 2 /*IN_MODIFY*/, 0, 0));
    // xattr family: the path (arg0) and the attribute name (arg1) are C strings the handler resolves/copies
    // before any host syscall. A wild pointer to either must surface the kernel's EFAULT, not fault the
    // engine's own path walker / name snprintf (which killed the guest with sig=11 before the guard). A real
    // backing file makes the name-pointer probe reach the copy step. The value buffer (arg2) is checked by
    // the host set/get, so its bad-pointer case is already EFAULT and is covered by setxattr_badval.
    int xf = open("/tmp/.efault_xattr_probe", O_CREAT | O_RDWR, 0644);
    (void)xf;
    printf("setxattr_badpath=%d\n", probe(SYS_setxattr, BAD, (long)"user.x", (long)"v", 1, 0));
    printf("setxattr_badname=%d\n", probe(SYS_setxattr, (long)"/tmp/.efault_xattr_probe", BAD, (long)"v", 1, 0));
    printf("setxattr_badval=%d\n",
           probe(SYS_setxattr, (long)"/tmp/.efault_xattr_probe", (long)"user.x", BAD, 4, 0));
    printf("getxattr_badname=%d\n",
           probe(SYS_getxattr, (long)"/tmp/.efault_xattr_probe", BAD, (long)"v", 4, 0));
    printf("removexattr_badname=%d\n", probe(SYS_removexattr, (long)"/tmp/.efault_xattr_probe", BAD, 0, 0, 0));
    // sendmsg/recvmsg with a NULL or wild msghdr pointer: on a real socket fd the kernel reaches the
    // copy_from_user of the msghdr and returns EFAULT. A handler that dereferences the msghdr struct
    // (iov ptr/count, control, flags) before validating it faults the engine (sig=11) instead. Uses a
    // datagram socket so the fd passes sockfd_lookup and the EFAULT is on the msghdr, not ENOTSOCK/EBADF.
    int sk = socket(AF_INET, SOCK_DGRAM, 0);
    printf("sendmsg_nullhdr=%d\n", probe(SYS_sendmsg, sk, 0, 0, 0, 0));
    printf("sendmsg_badhdr=%d\n", probe(SYS_sendmsg, sk, BAD, 0, 0, 0));
    printf("recvmsg_nullhdr=%d\n", probe(SYS_recvmsg, sk, 0, 0, 0, 0));
    printf("recvmsg_badhdr=%d\n", probe(SYS_recvmsg, sk, BAD, 0, 0, 0));
    // wait4 with a wild status pointer: the kernel reaps the zombie, THEN faults on the put_user of the
    // status word (a re-wait returns ECHILD -- the child is already gone), so the syscall returns EFAULT.
    // A handler that writes the status through the guest pointer without validating it faults the engine
    // (sig=11) instead. Needs a real reapable child, so this probe forks a grandchild rather than using
    // the shared helper (whose child has none to wait on -> ECHILD, not EFAULT).
    {
        pid_t w = fork();
        if (w == 0) {
            pid_t g = fork();
            if (g == 0) _exit(3);
            long r = syscall(SYS_wait4, g, BAD, 0, 0, 0);
            _exit(r == -1 ? (errno & 0x7f) : 0);
        }
        int st = 0;
        waitpid(w, &st, 0);
        printf("wait4_badstatus=%d\n", WIFSIGNALED(st) ? -WTERMSIG(st) : WEXITSTATUS(st));
    }
    return 0;
}
