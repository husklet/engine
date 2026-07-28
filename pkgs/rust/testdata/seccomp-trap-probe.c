#define _GNU_SOURCE

#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

static volatile sig_atomic_t trapped;
static volatile sig_atomic_t code;
static volatile sig_atomic_t error;
static volatile sig_atomic_t syscall_number;
static volatile uint32_t architecture;
static volatile uintptr_t call_address;
static volatile uintptr_t context_address;
static volatile uintptr_t context_syscall;

static void on_sigsys(int signal, siginfo_t *info, void *context) {
    if (signal != SIGSYS || info == NULL || context == NULL) _exit(20);
    ucontext_t *ucontext = context;
    trapped = 1;
    code = info->si_code;
    error = info->si_errno;
    syscall_number = info->si_syscall;
    architecture = info->si_arch;
    call_address = (uintptr_t)info->si_call_addr;
    context_address = (uintptr_t)ucontext->uc_mcontext.pc;
    context_syscall = (uintptr_t)ucontext->uc_mcontext.regs[8];
}

int main(void) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) != 0) return 8;
    if ((fcntl(pair[0], F_GETFL) & 0x20000) != 0) return 9;
    close(pair[0]);
    close(pair[1]);

    struct sigaction action = {
        .sa_sigaction = on_sigsys,
        .sa_flags = SA_SIGINFO,
    };
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSYS, &action, NULL) != 0) return 10;

    struct sock_filter instructions[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getpid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP | 0x1234),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program = {
        .len = (unsigned short)(sizeof instructions / sizeof instructions[0]),
        .filter = instructions,
    };

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return 11;
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) return 12;
    (void)syscall(SYS_getpid);

    if (!trapped) return 13;
    if (code != SYS_SECCOMP) return 14;
    if (syscall_number != SYS_getpid) return 15;
    if (architecture != AUDIT_ARCH_AARCH64) return 16;
    if (call_address == 0) return 17;
    if (call_address != context_address) return 19;
    if (context_syscall != SYS_getpid) return 21;
    if (error == 0) return 18;
    if (error != 0x1234) return 22;
    return 0;
}
