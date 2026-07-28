#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "hl/activation.h"
#include "../host/fork_wire.h"
#include "../host/system.h"
#include "provider/client.h"
#include "provider/files.h"
#include "provider/namespace.h"
#include "checkpoint_channel.h"
#include "engine_backend.h"
#include "environment.h"
#include "launch.h"
#include "../linux_abi/dns.h"
#include "../host/system.h"
#include "hl/config.h"
#if defined(__APPLE__)
#include "hl/macos.h"
typedef hl_host_macos hl_activation_host;
#elif defined(__linux__)
#include "hl/linux.h"
typedef hl_host_linux hl_activation_host;
#elif defined(_WIN32)
#include "hl/windows.h"
typedef hl_host_windows hl_activation_host;
#else
#error unsupported activation host
#endif

/*
 * WHAT THE WINDOWS ARM OF THIS FILE IS, AND WHAT IT IS NOT.
 *
 * Activation is two things wearing one name. The larger half is a PROTOCOL: a
 * magic/ABI/nonce-stamped request, a reply the parent must see before it hands
 * the embedder a live handle, a commit byte that arms the guest only after the
 * embedder has acknowledged, and a second reply carrying the guest's exit. None
 * of that is POSIX. It is byte layout, sequencing and validation, and it is
 * shared verbatim below -- one struct, one validator, one child body, one wait.
 *
 * The smaller half is a MECHANISM, and it is four POSIX facilities that Windows
 * does not have. Each one is replaced here rather than emulated, and the
 * replacement is named at its site:
 *
 *   fork/exec + posix_spawn   -> CreateProcessW of our own image, suspended.
 *   SCM_RIGHTS                -> DuplicateHandle into an explicit inheritance
 *                                set (PROC_THREAD_ATTRIBUTE_HANDLE_LIST). The
 *                                handle VALUES then travel as ordinary bytes in
 *                                the request, because an inherited handle keeps
 *                                its numeric slot in the child (measured).
 *   AF_UNIX socketpair        -> one duplex named pipe, wrapped in a CRT
 *                                descriptor so read/write/poll/close -- and
 *                                therefore transfer(), try_wait() and the whole
 *                                child body -- are the same code on both hosts.
 *   kill(-pid, SIGKILL)       -> a job object. A job is the only Windows object
 *                                with process-GROUP kill semantics, and unlike a
 *                                process group it also owns descendants that
 *                                changed group.
 *
 * Two things are genuinely lost rather than replaced, and they refuse with a
 * type rather than pretending:
 *
 *   - a controlling terminal. There is no setsid(), no TIOCSCTTY, and no object
 *     that is simultaneously the child's tty and one descriptor the parent can
 *     read and write. ConPTY is close but is a console emulator with its own VT
 *     translation on two separate pipes, which is a different object, not this
 *     one. The terminal forms return HL_STATUS_NOT_SUPPORTED.
 *   - catchable signals other than interrupt. Windows can deliver exactly two
 *     asynchronous notifications to another process: CTRL_C_EVENT and
 *     CTRL_BREAK_EVENT, both only within a process group and only to a console
 *     application. Those two are relayed to the guest as SIGINT and SIGQUIT.
 *     SIGHUP, SIGTERM, SIGUSR1 and SIGUSR2 have no delivery path at all.
 */
#if defined(_WIN32)
#include "../host/windows/win32.h"
/* arc4random_buf's Windows arm (rand_s), alongside this layer's other residual
 * system names. Included only here so the POSIX preprocessor input is unchanged. */
#include "../linux_abi/host_system.h"
#include <io.h>
#include <process.h>
#endif

#include <errno.h>
#if !defined(_WIN32)
#include <dirent.h>
#endif
#include <fcntl.h>
#include "../linux_abi/host_poll.h"
#include <pthread.h>
#include <signal.h>
#if !defined(_WIN32)
#include <spawn.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include "../linux_abi/host_socket.h"
#include "../linux_abi/host_tty.h"
#endif
#include "../linux_abi/host_wait.h"
#if defined(__APPLE__)
#include <util.h>
#elif !defined(_WIN32)
#include <pty.h>
#endif
#include <unistd.h>

static hl_status activation_host_create(hl_activation_host **host, hl_host_services *services) {
#if defined(__APPLE__)
    return hl_host_macos_create(host, services);
#elif defined(_WIN32)
    return hl_host_windows_create(host, services);
#else
    return hl_host_linux_create(host, services);
#endif
}

static void activation_host_destroy(hl_activation_host *host) {
#if defined(__APPLE__)
    hl_host_macos_destroy(host);
#elif defined(_WIN32)
    hl_host_windows_destroy(host);
#else
    hl_host_linux_destroy(host);
#endif
}

/*
 * "Absolute path" as each host spells it. The POSIX arm's leading-slash test is
 * the whole rule there and stays exactly that; on Windows a rooted path is a
 * drive letter followed by a colon and a separator, or a UNC share. The
 * leading-separator form is accepted on both because engine-internal callers
 * construct paths that way and the host file layer resolves them.
 */
static int activation_absolute(const char *path) {
    if (path == NULL) return 0;
#if defined(_WIN32)
    if (path[0] == '/' || path[0] == '\\') return 1;
    return ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':' &&
           (path[2] == '\\' || path[2] == '/');
#else
    return path[0] == '/';
#endif
}

#if !defined(_WIN32)
/* The child's environment is built from this one on POSIX. The Windows arm
 * builds a UTF-16 block from GetEnvironmentStringsW instead, and must not
 * declare this: the UCRT spells the same table `_environ` behind a macro named
 * `environ`, so the declaration expands into a redeclaration of an imported CRT
 * accessor rather than into an extern of a variable. */
extern char **environ;
#endif
void hl_activation_test_mode(uint32_t mode);

enum { HL_ACTIVATION_FD = 3, HL_ACTIVATION_ABI = 2, HL_ACTIVATION_PATH_MAX = 4096 };

/* Descriptor roles carried by one activation request, in ascending bit order. ABI 1 carried at most the
 * provider transport in an untagged single slot; ABI 2 tags them so the checkpoint broker can be attached
 * with or without a provider. */
enum { HL_ACTIVATION_ROLE_TRANSPORT = 1u, HL_ACTIVATION_ROLE_CHECKPOINT = 2u, HL_ACTIVATION_ROLE_TRIGGER = 4u };

#define HL_ACTIVATION_MAGIC UINT64_C(0x484c414354495631)

typedef struct hl_activation_request {
    uint64_t magic;
    uint32_t abi;
    uint32_t size;
    uint64_t nonce[2];
    uint32_t guest_isa;
    uint32_t path_size;
    uint32_t test_flags;
    uint32_t reserved;         /* number of attached descriptors */
    uint32_t descriptor_roles; /* HL_ACTIVATION_ROLE_* bitmask; descriptors arrive in ascending bit order */
    uint32_t reserved_abi2;
    char path[HL_ACTIVATION_PATH_MAX];
#if defined(_WIN32)
    /* The SCM_RIGHTS replacement. The parent puts each attached handle in the
     * child's inheritance set, which fixes its numeric value there, and then
     * names those values here in the same ascending role-bit order the POSIX arm
     * sends its descriptors. The field is inside the Windows arm so the POSIX
     * wire layout -- and therefore sizeof(request), which both sides check -- is
     * byte-identical to what it has always been. */
    uint64_t handles[3];
#endif
} hl_activation_request;

typedef struct hl_activation_reply {
    uint64_t magic;
    uint32_t abi;
    uint32_t size;
    uint64_t nonce[2];
    int32_t status;
    uint32_t reserved;
    hl_engine_exit result;
} hl_activation_reply;

static int transfer(int fd, void *data, size_t size, int writing) {
    unsigned char *bytes = data;
    size_t offset = 0;
    while (offset < size) {
#if defined(_WIN32)
        /* The control channel is a named pipe behind a CRT descriptor, so the
         * plain read/write pair is the whole transport: there is no send(), and
         * there is no SIGPIPE for MSG_NOSIGNAL to suppress -- a write to a pipe
         * whose reader is gone fails with ERROR_BROKEN_PIPE, which is the EPIPE
         * this loop already treats as a hard stop. The chunk is capped at INT_MAX
         * because the CRT's count argument is an unsigned int. */
        unsigned int chunk = size - offset > 0x40000000u ? 0x40000000u : (unsigned int)(size - offset);
        int count = writing ? _write(fd, bytes + offset, chunk) : _read(fd, bytes + offset, chunk);
#else
        int flags = 0;
#if defined(MSG_NOSIGNAL)
        if (writing) flags = MSG_NOSIGNAL;
#endif
        ssize_t count =
            writing ? send(fd, bytes + offset, size - offset, flags) : read(fd, bytes + offset, size - offset);
#endif
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        offset += (size_t)count;
    }
    return 0;
}

static int activation_provider_handshake(int descriptor) {
    unsigned char hello[32] = {'R', 'P', 'L', 'H', 1, 0, 1, 0};
    unsigned char ready[32], setup[32], acknowledged[32] = {'R', 'P', 'L', 'H', 1, 0, 8, 0};
    unsigned char *payload = NULL;
    struct pollfd pollfd = {.fd = descriptor, .events = POLLIN | POLLHUP};
    int result;
    hello[20] = 1; /* NamespaceInstall v1. */
    if (transfer(descriptor, hello, sizeof(hello), 1) != 0) return -1;
    do {
        result = poll(&pollfd, 1, 5000);
    } while (result < 0 && errno == EINTR);
    if (result <= 0 || (pollfd.revents & POLLIN) == 0 || transfer(descriptor, ready, sizeof(ready), 0) != 0) return -1;
    /* HLPR, version 1, READY, empty payload, request id zero, reserved zero. */
    if (ready[0] != 'R' || ready[1] != 'P' || ready[2] != 'L' || ready[3] != 'H' || ready[4] != 1 || ready[5] != 0 ||
        ready[6] != 2 || ready[7] != 0)
        return -1;
    for (size_t index = 8; index < 20; ++index)
        if (ready[index] != 0) return -1;
    for (size_t index = 28; index < sizeof(ready); ++index)
        if (ready[index] != 0) return -1;
    if ((ready[20] & 1u) != 0) {
        uint32_t size;
        if (transfer(descriptor, setup, sizeof(setup), 0) != 0 || setup[0] != 'R' || setup[1] != 'P' ||
            setup[2] != 'L' || setup[3] != 'H' || setup[4] != 1 || setup[5] != 0 || setup[6] != 7 || setup[7] != 0)
            return -1;
        size = (uint32_t)setup[8] | (uint32_t)setup[9] << 8 | (uint32_t)setup[10] << 16 | (uint32_t)setup[11] << 24;
        if (size > 1024 * 1024 || memcmp(setup + 12, "\0\0\0\0\0\0\0\0", 8) != 0) return -1;
        payload = malloc(size == 0 ? 1 : size);
        if (payload == NULL || (size != 0 && transfer(descriptor, payload, size, 0) != 0) ||
            hl_provider_namespace_launch_install(payload, size) != 0) {
            free(payload);
            return -1;
        }
        free(payload);
        if (transfer(descriptor, acknowledged, sizeof(acknowledged), 1) != 0) return -1;
    }
    return 0;
}

/*
 * Bringing this archive's guest targets up, which is a different question on a
 * one-target archive than on a two-target one.
 *
 * src/core/target/namespace.h decides the names: an archive carrying BOTH ISAs
 * sets HL_TARGET_NAMESPACE per translator so the two unity translation units can
 * coexist, which is what produces hl_aarch64_* and hl_x86_64_*; an archive
 * carrying ONE leaves it unset, and that translator keeps the historical
 * un-namespaced hl_target_* names declared in engine_backend.h. Both POSIX
 * production archives are the first kind. The Windows archive is the second --
 * one unity TU, x86-64 -- so naming the prefixed symbols here would reference
 * something that is not merely absent but was never spelled that way.
 *
 * The ISA argument therefore has no work to do on a one-target archive: there is
 * exactly one backend and it registers itself under its own ISA. A guest that
 * asks for a different one is refused by hl_engine_create, which finds no
 * backend for that ISA and returns a real status -- and that status travels back
 * to the embedder in the activation reply. That is the honest failure for "this
 * engine does not carry that architecture", and it is reported by the component
 * that owns the answer rather than guessed at here.
 */
#if defined(_WIN32)
/* hl_target_register_backend and hl_target_runtime_init come from engine_backend.h. */
#else
void hl_aarch64_target_register_backend(void);
void hl_x86_64_target_register_backend(void);
void hl_aarch64_target_runtime_init(void);
void hl_x86_64_target_runtime_init(void);
#endif
void hl_host_private_init(void);
void hl_fdcache_runtime_init(void);

static void hl_embedded_runtime_init(uint32_t guest_isa) {
    hl_host_private_init();
    hl_fdcache_runtime_init();
#if defined(_WIN32)
    (void)guest_isa;
    hl_target_register_backend();
    hl_target_runtime_init();
#else
    hl_aarch64_target_register_backend();
    hl_x86_64_target_register_backend();
    if (guest_isa == HL_GUEST_ISA_AARCH64)
        hl_aarch64_target_runtime_init();
    else
        hl_x86_64_target_runtime_init();
#endif
}

static hl_host_services *activation_services;
static uint32_t activation_guest_isa;
static hl_engine_exit *activation_result;
static hl_status activation_status;
static hl_engine *activation_engine;
static hl_provider_client activation_provider_client;
static pthread_mutex_t activation_engine_lock = PTHREAD_MUTEX_INITIALIZER;
static int activation_signal_pipe[2] = {-1, -1};
static uint32_t activation_pending_signal;

/*
 * The activation constructor is the first engine code in the exec'd child.
 * Make descriptor isolation an invariant here as well as a launcher policy:
 * embedders may use a different spawn implementation, and platform spawn
 * extensions must never decide what becomes guest-visible state.
 */
static void activation_close_unrelated_descriptors(void) {
#if defined(_WIN32)
    /* Nothing to close, and that is a property of the spawn rather than an
     * omission. CreateProcessW inherits only the handles named in the launch's
     * PROC_THREAD_ATTRIBUTE_HANDLE_LIST, so the isolation this function performs
     * after the fact on POSIX has already happened -- atomically, and before the
     * child had an address space to observe it from. There is also no ambient
     * descriptor table to sweep: a CRT descriptor is a per-process table entry
     * that a child builds itself, not an inherited kernel object. */
#elif defined(__linux__)
    closefrom(HL_ACTIVATION_FD + 1);
#else
    int limit = getdtablesize();
    for (int fd = HL_ACTIVATION_FD + 1; fd < limit; ++fd)
        (void)close(fd);
#endif
}

static int activation_guest_signal(int host_signal) {
#if defined(__linux__) || defined(_WIN32)
    /* The Windows relay never sees a host signal number: its two sources are
     * console control events, which activation_console_handler has already
     * turned into the guest numbers the engine speaks. Identity here keeps that
     * one conversion in one place. */
    return host_signal;
#else
    static const unsigned char macos_to_linux[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  7,  11, 31, 13, 14, 15,
                                                     23, 19, 20, 18, 17, 21, 22, 29, 24, 25, 26, 27, 28, 29, 10, 12};
    return host_signal > 0 && host_signal < 32 ? macos_to_linux[host_signal] : host_signal;
#endif
}

static void activation_signal_handler(int signal_number) {
    unsigned char guest = (unsigned char)activation_guest_signal(signal_number);
    if (activation_signal_pipe[1] >= 0) {
#if defined(_WIN32)
        (void)_write(activation_signal_pipe[1], &guest, 1u);
#else
        ssize_t ignored = write(activation_signal_pipe[1], &guest, sizeof(guest));
        (void)ignored;
#endif
    }
}

static void *activation_signal_relay(void *unused) {
    unsigned char signal_number;
    (void)unused;
#if defined(_WIN32)
    while (_read(activation_signal_pipe[0], &signal_number, 1u) == 1) {
#else
    while (read(activation_signal_pipe[0], &signal_number, sizeof(signal_number)) == sizeof(signal_number)) {
#endif
        uint32_t guest_signal = signal_number;
        if (signal_number == 0) break;
        pthread_mutex_lock(&activation_engine_lock);
        if (activation_engine != NULL)
            (void)hl_engine_request(activation_engine, HL_ENGINE_REQUEST_SIGNAL, &guest_signal, sizeof(guest_signal));
        else
            activation_pending_signal = signal_number;
        pthread_mutex_unlock(&activation_engine_lock);
    }

    return NULL;
}

#if !defined(_WIN32)
static const int activation_forwarded[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGUSR1, SIGUSR2};
static volatile sig_atomic_t activation_relay_detached;
#endif

/* The guest is a fork of this process and has no relay thread.  An inherited relay handler
 * makes the relay's kill() write into the shared self-pipe instead of acting on the guest:
 * a default-disposition guest is never terminated and the relay reissues the same signal
 * forever.  Restore the default disposition in the child; a guest that wants a handler
 * installs its own through rt_sigaction.  The flag is inherited as 1, so a fork by an
 * already-detached guest keeps whatever dispositions that guest chose. */
#if !defined(_WIN32)
static void activation_signal_relay_fork_child(void) {
    size_t index;
    if (activation_relay_detached) return;
    activation_relay_detached = 1;
    for (index = 0; index < sizeof(activation_forwarded) / sizeof(activation_forwarded[0]); ++index)
        (void)signal(activation_forwarded[index], SIG_DFL);
}
#endif

/* The private descriptor band is a hoist, not a capability: hosts that have one
 * move the engine's own descriptors above the range a guest can name, and hosts
 * that do not have one (Windows: no F_DUPFD, no per-process descriptor rlimit)
 * report a floor of -1 and refuse every registration. Failing the relay for that
 * would refuse to run a guest over a missing collision guard, so the refusal is
 * tolerated where the band is absent and remains an error where it exists. */
static int activation_private_add(int descriptor) {
    if (hl_host_process_fd_private_add(descriptor) == 0) return 0;
    return hl_host_process_fd_private_floor() < 0 ? 0 : -1;
}

#if defined(_WIN32)
/*
 * The Windows source for the relay. Console control events are the only
 * asynchronous notification another process can deliver here, and there are
 * exactly two of them; both arrive on a thread the OS injects into this process,
 * so unlike a POSIX signal handler this runs with no async-signal-safety
 * constraint. It still writes to the self-pipe rather than calling the engine
 * directly, because the engine call must not run on a thread the console
 * subsystem will tear down when the handler returns.
 *
 * CLOSE/LOGOFF/SHUTDOWN are deliberately NOT claimed. Returning TRUE for them
 * would suppress the OS's own termination path while the guest was still
 * running, and there is no guest signal whose semantics match "you have five
 * seconds before you are killed regardless".
 */
static BOOL WINAPI activation_console_handler(DWORD type) {
    /* Guest (Linux) numbers, not host numbers: SIGINT for Ctrl+C, SIGQUIT for
     * Ctrl+Break, which is the pairing every Windows console tool uses. */
    int guest = type == CTRL_C_EVENT ? 2 : type == CTRL_BREAK_EVENT ? 3 : 0;
    if (guest == 0) return FALSE;
    activation_signal_handler(guest);
    return TRUE;
}
#endif

static int activation_signal_relay_start(pthread_t *thread) {
#if !defined(_WIN32)
    struct sigaction action;
    size_t index;
    int flags;
#endif
#if defined(_WIN32)
    /* _pipe is the CRT's anonymous pipe: two descriptors over one kernel pipe,
     * which is exactly what the relay needs and what pipe(2) gives elsewhere. */
    if (_pipe(activation_signal_pipe, 4096, 0) != 0) return -1;
#else
    if (pipe(activation_signal_pipe) != 0) return -1;
#endif
    if (activation_private_add(activation_signal_pipe[0]) != 0 ||
        activation_private_add(activation_signal_pipe[1]) != 0) {
        hl_host_process_fd_private_remove(activation_signal_pipe[0]);
        hl_host_process_fd_private_remove(activation_signal_pipe[1]);
        close(activation_signal_pipe[0]);
        close(activation_signal_pipe[1]);
        activation_signal_pipe[0] = -1;
        activation_signal_pipe[1] = -1;
        return -1;
    }
#if defined(_WIN32)
    /* No O_NONBLOCK equivalent for a CRT descriptor, and none is needed: the
     * handler runs on its own thread and writes one byte into a 4096-byte pipe
     * buffer, so the write that O_NONBLOCK exists to keep off a signal handler's
     * stack cannot block here even if the relay were arbitrarily slow. */
    if (!SetConsoleCtrlHandler(activation_console_handler, TRUE)) goto fail;
    if (pthread_create(thread, NULL, activation_signal_relay, NULL) == 0) return 0;
    (void)SetConsoleCtrlHandler(activation_console_handler, FALSE);
#else
    flags = fcntl(activation_signal_pipe[1], F_GETFL);
    if (flags < 0 || fcntl(activation_signal_pipe[1], F_SETFL, flags | O_NONBLOCK) != 0) goto fail;
    memset(&action, 0, sizeof(action));
    action.sa_handler = activation_signal_handler;
    action.sa_flags = SA_RESTART;
    sigfillset(&action.sa_mask);
    for (index = 0; index < sizeof(activation_forwarded) / sizeof(activation_forwarded[0]); ++index)
        if (sigaction(activation_forwarded[index], &action, NULL) != 0) goto fail;
    if (pthread_atfork(NULL, NULL, activation_signal_relay_fork_child) != 0) goto fail;
    if (pthread_create(thread, NULL, activation_signal_relay, NULL) == 0) return 0;
#endif
fail:
    hl_host_process_fd_private_remove(activation_signal_pipe[0]);
    hl_host_process_fd_private_remove(activation_signal_pipe[1]);
    close(activation_signal_pipe[0]);
    close(activation_signal_pipe[1]);
    activation_signal_pipe[0] = -1;
    activation_signal_pipe[1] = -1;
    return -1;
}

static void activation_signal_relay_stop(pthread_t thread) {
    unsigned char stop = 0;
#if defined(_WIN32)
    (void)SetConsoleCtrlHandler(activation_console_handler, FALSE);
    (void)_write(activation_signal_pipe[1], &stop, 1u);
#else
    ssize_t ignored = write(activation_signal_pipe[1], &stop, sizeof(stop));
    (void)ignored;
#endif
    (void)pthread_join(thread, NULL);
    hl_host_process_fd_private_remove(activation_signal_pipe[0]);
    hl_host_process_fd_private_remove(activation_signal_pipe[1]);
    close(activation_signal_pipe[0]);
    close(activation_signal_pipe[1]);
    activation_signal_pipe[0] = -1;
    activation_signal_pipe[1] = -1;
}

static int activation_run_config(const char *rootfs, const char *executable_host, uint32_t argc, char *const argv[],
                                 const hl_options *options, const char *result_path) {
    hl_engine_fd_binding bindings[3] = {0};
    hl_engine_executable executable = {0};
    hl_engine_config config = {
        .abi = HL_ENGINE_ABI, .size = sizeof(config), .guest_isa = activation_guest_isa, .rootfs = rootfs};
    hl_engine *engine = NULL;
    uint32_t count = 0;
    uint32_t stream;
    (void)result_path;
    for (stream = 0; stream < 3; ++stream) {
        hl_host_result adopted = activation_services->file->standard_stream(activation_services->context, stream);
        uint32_t access;
        if (adopted.status == HL_STATUS_NOT_FOUND) continue;
        if (adopted.status != HL_STATUS_OK) {
            activation_status = (hl_status)adopted.status;
            return 78;
        }
        access = (uint32_t)adopted.detail & (HL_HOST_FILE_READ | HL_HOST_FILE_WRITE);
        bindings[count] =
            (hl_engine_fd_binding){.abi = HL_ENGINE_ABI,
                                   .size = sizeof(bindings[count]),
                                   .guest_fd = stream,
                                   .status_flags = access == (HL_HOST_FILE_READ | HL_HOST_FILE_WRITE) ? HL_LINUX_O_RDWR
                                                   : access == HL_HOST_FILE_WRITE ? HL_LINUX_O_WRONLY
                                                                                  : HL_LINUX_O_RDONLY,
                                   .ownership = HL_ENGINE_FD_TRANSFER,
                                   .host_handle = adopted.value};
        if (((uint32_t)adopted.detail & HL_HOST_FILE_APPEND) != 0) bindings[count].status_flags |= HL_LINUX_O_APPEND;
        if (((uint32_t)adopted.detail & HL_HOST_FILE_NONBLOCK) != 0)
            bindings[count].status_flags |= HL_LINUX_O_NONBLOCK;
        ++count;
    }
    config.fd_bindings = bindings;
    config.fd_binding_count = count;
    if (executable_host != NULL) {
        /* Same contract as the CLI entry path: the launcher names a program, and execve(2) follows a
           program symlink. NOFOLLOW made every symlinked entry program fail the launch with ELOOP. */
        hl_host_result opened =
            activation_services->file->open_relative(activation_services->context, HL_HOST_HANDLE_CWD, executable_host,
                                                     strlen(executable_host), HL_HOST_FILE_READ, 0, 0);
        if (opened.status != HL_STATUS_OK) {
            activation_status = (hl_status)opened.status;
            return 78;
        }
        executable =
            (hl_engine_executable){HL_ENGINE_ABI, sizeof(executable), HL_ENGINE_FD_TRANSFER, 0, opened.value, NULL, 0};
        config.executable = &executable;
    }
    activation_status = hl_engine_create_with_options(&config, activation_services, options, &engine);
    if (activation_status != HL_STATUS_OK && config.executable != NULL)
        (void)activation_services->file->close(activation_services->context, executable.host_handle);
    if (activation_status == HL_STATUS_OK) {
        uint32_t pending;
        pthread_mutex_lock(&activation_engine_lock);
        activation_engine = engine;
        pending = activation_pending_signal;
        activation_pending_signal = 0;
        if (pending != 0) (void)hl_engine_request(engine, HL_ENGINE_REQUEST_SIGNAL, &pending, sizeof(pending));
        pthread_mutex_unlock(&activation_engine_lock);
        activation_status = hl_engine_run(engine, (int)argc, (const char *const *)argv, activation_result);
        pthread_mutex_lock(&activation_engine_lock);
        activation_engine = NULL;
        pthread_mutex_unlock(&activation_engine_lock);
    }
    hl_engine_destroy(engine);
    return activation_status == HL_STATUS_OK ? 0 : 78;
}

static void hl_activation_child(void) {
    hl_activation_request request;
    hl_activation_reply reply = {0};
    hl_activation_host *host = NULL;
    hl_host_services services;
    long descriptor;
    hl_status status = HL_STATUS_CORRUPT;
    unsigned char commit;
    pthread_t signal_thread;
    int signal_relay = 0;
    int inherited[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int inherited_count = 0;
    int transport_descriptor = -1;
    int checkpoint_descriptor = -1;
    int trigger_descriptor = -1;
    int environment = hl_environment_take_activation_descriptor(&descriptor);
    if (environment == 0) return;
#if defined(_WIN32)
    /* HL_ACTIVATION_FD carries an inherited HANDLE value here rather than the
     * fixed descriptor number 3. There is no dup2 to place a handle at a chosen
     * slot on this host, and there is no need for one: an inherited handle keeps
     * the numeric value it had in the parent, so naming that value is exactly as
     * definite as naming slot 3 was. The CRT descriptor built over it is what
     * makes every read/write/poll below identical to the POSIX arm's, and it
     * takes ownership -- close(descriptor) later closes the handle. */
    if (environment < 0 || descriptor <= 0) _exit(125);
    {
        int opened = _open_osfhandle((intptr_t)(uintptr_t)(unsigned long)descriptor, 0);
        if (opened < 0) _exit(125);
        descriptor = opened;
    }
    /* CREATE_NEW_PROCESS_GROUP starts a child with Ctrl+C ignored. The relay
     * installs a handler for it below, so restore the inheritable disposition
     * first; without this the handler is installed onto an event the process is
     * configured never to receive. */
    (void)SetConsoleCtrlHandler(NULL, FALSE);
#else
    if (environment < 0 || descriptor != HL_ACTIVATION_FD) _exit(125);
#endif
#if defined(__APPLE__)
    /* Foundation's concrete string classes must finish initialization before
     * activation creates its relay thread and the host backend forks a guest.
     * The warmup opens a com.apple.netsrc control socket, so descriptor
     * isolation must run after it rather than letting that socket become guest
     * state. */
    hl_linux_dns_prepare();
#endif
    activation_close_unrelated_descriptors();
    /* Embedded builds deliberately omit the native constructor.  Transport
     * adoption needs the private descriptor registry before the later backend
     * initialization boundary, otherwise every attached provider fails with
     * ENOSPC before it can send HELLO. */
    hl_host_private_init();
    /* The fixed activation socket remains open while the guest runs.  Keep it
     * out of the guest descriptor registry (and therefore checkpoints); it is
     * engine control state, not a Linux guest socket. */
    if (activation_private_add((int)descriptor) != 0) _exit(126);
#if defined(_WIN32)
    /* The SCM_RIGHTS receive, in the shape this host has. The request is ordinary
     * bytes over the control pipe; the attached handles arrived earlier and by a
     * different route -- CreateProcessW's inheritance set -- so all that is left
     * here is to turn the values the request names into CRT descriptors, in the
     * same ascending role-bit order the sender used. A value the parent did not
     * attach is zero, which is never a live Win32 handle. */
    if (transfer((int)descriptor, &request, sizeof(request), 0) != 0) _exit(126);
    {
        unsigned attached = request.reserved > 3u ? 3u : request.reserved;
        for (unsigned index = 0; index < attached; ++index) {
            int opened;
            if (request.handles[index] == 0 || request.handles[index] > (uint64_t)INT32_MAX) _exit(126);
            opened = _open_osfhandle((intptr_t)(uintptr_t)request.handles[index], 0);
            if (opened < 0) _exit(126);
            inherited[inherited_count++] = opened;
        }
    }
#else
    if (hl_fork_wire_receive_descriptors((int)descriptor, &request, sizeof(request), inherited, &inherited_count) !=
        (int)sizeof(request))
        _exit(126);
#endif
    {
        unsigned roles = request.descriptor_roles;
        int expected = ((roles & HL_ACTIVATION_ROLE_TRANSPORT) != 0) + ((roles & HL_ACTIVATION_ROLE_CHECKPOINT) != 0) +
                       ((roles & HL_ACTIVATION_ROLE_TRIGGER) != 0);
        int slot = 0;
        if (request.reserved > 3u ||
            (roles & ~(unsigned)(HL_ACTIVATION_ROLE_TRANSPORT | HL_ACTIVATION_ROLE_CHECKPOINT |
                                 HL_ACTIVATION_ROLE_TRIGGER)) != 0 ||
            inherited_count != (int)request.reserved || inherited_count != expected) {
            while (inherited_count > 0)
                (void)close(inherited[--inherited_count]);
            _exit(126);
        }
        for (int index = 0; index < inherited_count; ++index) {
            /* Same tolerance as activation_private_add, and for the same reason:
             * a host with no private band leaves the descriptor where it is
             * rather than losing it (the contract says a failed adopt does not
             * take ownership), and the guest collision the hoist prevents is a
             * hazard rather than a correctness claim. */
            int adopted = hl_host_process_fd_private_adopt(inherited[index]);
            if (adopted < 0 && hl_host_process_fd_private_floor() >= 0) _exit(126);
            if (adopted >= 0) inherited[index] = adopted;
        }
        /* Re-seat by role: `transport` keeps the historical slot 0 meaning, `checkpoint` is the broker every
         * later fork() of this process uses to reach the embedder's checkpoint store. */
        if ((roles & HL_ACTIVATION_ROLE_TRANSPORT) != 0) transport_descriptor = inherited[slot++];
        if ((roles & HL_ACTIVATION_ROLE_CHECKPOINT) != 0) checkpoint_descriptor = inherited[slot++];
        if ((roles & HL_ACTIVATION_ROLE_TRIGGER) != 0) trigger_descriptor = inherited[slot++];
        inherited[0] = transport_descriptor;
        inherited[1] = -1;
        inherited[2] = -1;
        if (checkpoint_descriptor >= 0) hl_ckpt_channel_publish(checkpoint_descriptor);
        if (trigger_descriptor >= 0) hl_ckpt_trigger_publish(trigger_descriptor);
    }
    reply.magic = HL_ACTIVATION_MAGIC;
    reply.abi = HL_ACTIVATION_ABI;
    reply.size = sizeof(reply);
    reply.nonce[0] = request.nonce[0];
    reply.nonce[1] = request.nonce[1];
    reply.result.abi = HL_ENGINE_ABI;
    reply.result.size = sizeof(reply.result);
    if (request.test_flags == 1) reply.nonce[0] ^= UINT64_C(1);
    if (request.magic == HL_ACTIVATION_MAGIC && request.abi == HL_ACTIVATION_ABI && request.size == sizeof(request) &&
        request.path_size > 1 && request.path_size <= sizeof(request.path) && activation_absolute(request.path) &&
        request.path[request.path_size - 1] == 0 &&
        (request.guest_isa == HL_GUEST_ISA_AARCH64 || request.guest_isa == HL_GUEST_ISA_X86_64)) {
        /* Armed before the reply: activation_start returns to the embedder as soon as this
         * reply is acknowledged, and the embedder may signal immediately.  A signal landing
         * before the handler exists would hit SIG_DFL and kill the engine process outright,
         * never reaching the guest. */
        if (activation_signal_relay_start(&signal_thread) != 0) {
            reply.status = HL_STATUS_PLATFORM_FAILURE;
            (void)transfer((int)descriptor, &reply, sizeof(reply), 1);
            _exit(124);
        }
        signal_relay = 1;
        reply.status = HL_STATUS_OK;
        if (transfer((int)descriptor, &reply, sizeof(reply), 1) != 0) _exit(124);
        if (request.test_flags == 4) _exit(123);
        if (transfer((int)descriptor, &commit, 1, 0) != 0 || commit != 0xa5u) _exit(124);
        if (inherited[0] >= 0 && activation_provider_handshake(inherited[0]) != 0) _exit(124);
        /* Explicit setup is idempotent and independent of constructor order. */
        hl_embedded_runtime_init(request.guest_isa);
        hl_embedded_runtime_init(request.guest_isa);
        status = activation_host_create(&host, &services);
        if (status == HL_STATUS_OK && inherited[0] >= 0 &&
            (hl_provider_client_init(&activation_provider_client, inherited[0], 1024 * 1024) != 0 ||
             hl_provider_files_install(&services, &activation_provider_client) != 0))
            status = HL_STATUS_PLATFORM_FAILURE;
        activation_services = &services;
        activation_guest_isa = request.guest_isa;
        activation_result = &reply.result;
        activation_status = status;
        if (status == HL_STATUS_OK && hl_run_config_file_with(request.path, activation_run_config) != 0 &&
            activation_status == HL_STATUS_OK)
            activation_status = HL_STATUS_CORRUPT;
        status = activation_status;
    } else {
        (void)transfer((int)descriptor, &reply, sizeof(reply), 1);
        _exit(127);
    }
    reply.status = (int32_t)status;
    if (request.test_flags == 5) reply.nonce[0] ^= UINT64_C(1);
    (void)transfer((int)descriptor, &reply, sizeof(reply), 1);
    if (signal_relay) activation_signal_relay_stop(signal_thread);
    activation_host_destroy(host);
    if (inherited[0] >= 0) {
        hl_provider_files_revoke();
        hl_provider_namespace_launch_revoke();
        hl_provider_client_destroy(&activation_provider_client);
    }
    if (inherited[0] >= 0) {
        hl_host_process_fd_private_remove(inherited[0]);
        (void)close(inherited[0]);
    }
    if (trigger_descriptor >= 0) {
        hl_ckpt_trigger_publish(-1);
        hl_host_process_fd_private_remove(trigger_descriptor);
        (void)close(trigger_descriptor);
    }
    if (checkpoint_descriptor >= 0) {
        hl_ckpt_channel_publish(-1);
        hl_host_process_fd_private_remove(checkpoint_descriptor);
        (void)close(checkpoint_descriptor);
    }
    hl_host_process_fd_private_remove((int)descriptor);
    (void)close((int)descriptor);
    _exit(status == HL_STATUS_OK ? 0 : 127);
}

static uint32_t activation_test_mode;

void hl_activation_test_mode(uint32_t mode) {
    activation_test_mode = mode;
}

__attribute__((constructor)) static void hl_activation_constructor(void) {
    hl_activation_child();
}

struct hl_activation_process {
    int descriptor;
#if defined(_WIN32)
    unsigned long pid;
#else
    pid_t pid;
#endif
    uint64_t nonce[2];
    uint32_t finished;
    hl_status final_status;
    hl_engine_exit final_exit;
#if defined(_WIN32)
    /* `job` is this activation's kill(-pid, SIGKILL): every descendant of the
     * engine child is in it, including one that changed process group, which a
     * Windows process group would not have held. `domain_job` is the launch's
     * process domain when the config named one -- a NAMED job, so any process
     * that knows the identity can reach the same object without a handle.
     * `process` is retained only until the exit code has been read. */
    void *process;
    void *job;
    void *domain_job;
#endif
};

#if defined(_WIN32)

/* The exit code a forced termination stamps on its victims. It is the same
 * customer-defined NTSTATUS encoding the Windows host backend uses for a signal
 * death (0xE0484C00 + signal), so a guest killed through this path and one killed
 * through the host's own terminate() decode identically -- both read back as
 * "died of signal 9" rather than as an ordinary exit whose status happens to
 * collide. The customer bit (0x20000000) is what keeps that space disjoint from
 * every real NTSTATUS and from any value a main() could return. */
#define HL_ACTIVATION_WINDOWS_KILL_CODE (0xE0484C00u + 9u)

/*
 * A process domain on this host is a NAMED JOB OBJECT.
 *
 * On POSIX a domain is a directory of membership files under /tmp: each guest
 * process publishes its own pid and its start time there, and the two functions
 * below read that registry and verify each member against the live process table
 * before acting on it. Neither half of that exists here. hl_host_process_read is
 * a documented refusal on this host -- there is no supported interface that
 * reports another process's start time -- so the birth record could be written
 * but never checked, which would leave the pid-reuse hazard the birth record was
 * invented to close wide open.
 *
 * A job object closes it structurally instead of by verification. Membership is
 * a kernel relation rather than a file, a pid cannot be reused into a job, and
 * descendants join automatically at creation, so a double-forked guest that has
 * left every process group is still a member. That is a stronger guarantee than
 * the registry provides, and it is why this is a replacement rather than a port.
 *
 * The name is derived from the 128-bit identity so the domain is reachable by
 * value: the embedder generates the identity, activation names a job after it,
 * and domain_terminate opens the same name later with no shared handle. Local\
 * scopes it to the caller's logon session, which is the closest match to a
 * /tmp entry's reach.
 */
static void domain_object_name(hl_process_domain domain, char *name, size_t capacity) {
    snprintf(name, capacity, "Local\\hl.domain.%016llx%016llx", (unsigned long long)domain.identity[0],
             (unsigned long long)domain.identity[1]);
}

static HANDLE domain_job_open(hl_process_domain domain, int create) {
    char name[64];
    domain_object_name(domain, name, sizeof name);
    if (create) return CreateJobObjectA(NULL, name);
    return OpenJobObjectA(JOB_OBJECT_QUERY | JOB_OBJECT_TERMINATE | SYNCHRONIZE, FALSE, name);
}

/* The member snapshot. A job reports its whole membership in one call, but the
 * buffer must be sized for it, and the count can change between the sizing call
 * and the read -- so this grows and retries rather than trusting the first
 * answer. Returns the count, or a negative value on failure. `ids` may be NULL
 * to ask only for the count. */
static int domain_job_processes(HANDLE job, DWORD *ids, uint32_t capacity, uint32_t *out_total) {
    uint32_t room = capacity < 64u ? 64u : capacity;
    unsigned attempt;
    for (attempt = 0; attempt < 6u; ++attempt) {
        size_t bytes = sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + (size_t)room * sizeof(ULONG_PTR);
        JOBOBJECT_BASIC_PROCESS_ID_LIST *list = calloc(1, bytes);
        DWORD produced = 0;
        BOOL queried;
        if (list == NULL) return -1;
        queried = QueryInformationJobObject(job, JobObjectBasicProcessIdList, list, (DWORD)bytes, &produced);
        /* A truncated answer is reported as ERROR_MORE_DATA with the assigned
         * count still filled in, which is exactly the number to grow to. */
        if (!queried && GetLastError() != ERROR_MORE_DATA) {
            free(list);
            return -1;
        }
        if (!queried || list->NumberOfProcessIdsInList < list->NumberOfAssignedProcesses) {
            uint32_t needed = (uint32_t)list->NumberOfAssignedProcesses;
            free(list);
            if (needed <= room) needed = room * 2u;
            if (needed > 1u << 20) return -1;
            room = needed;
            continue;
        }
        {
            uint32_t total = (uint32_t)list->NumberOfProcessIdsInList;
            uint32_t index;
            if (ids != NULL)
                for (index = 0; index < total && index < capacity; ++index)
                    ids[index] = (DWORD)list->ProcessIdList[index];
            if (out_total != NULL) *out_total = total;
            free(list);
            return 0;
        }
    }
    return -1;
}

#endif /* _WIN32 */

#if !defined(_WIN32)
static void domain_path(hl_process_domain domain, char *path, size_t capacity) {
    snprintf(path, capacity, "/tmp/.hl-domain.%016llx%016llx", (unsigned long long)domain.identity[0],
             (unsigned long long)domain.identity[1]);
}

static int domain_birth(const char *directory, pid_t pid, uint64_t *birth) {
    char path[160], text[32], *end;
    unsigned long long value;
    ssize_t count;
    int descriptor;
    snprintf(path, sizeof path, "%s/b%d", directory, (int)pid);
    descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return 0;
    do {
        count = read(descriptor, text, sizeof text - 1);
    } while (count < 0 && errno == EINTR);
    (void)close(descriptor);
    if (count <= 0) return 0;
    text[count] = 0;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || (*end != '\n' && *end != 0) || value == 0) return 0;
    *birth = (uint64_t)value;
    return 1;
}

static void domain_record_remove(const char *directory, pid_t pid) {
    static const char prefixes[] = {'\0', 'b', 'x'};
    char path[160];
    size_t index;
    for (index = 0; index < sizeof prefixes; ++index) {
        if (prefixes[index] == 0)
            snprintf(path, sizeof path, "%s/%d", directory, (int)pid);
        else
            snprintf(path, sizeof path, "%s/%c%d", directory, prefixes[index], (int)pid);
        (void)unlink(path);
    }
}

static void domain_directory_remove(const char *directory) {
    DIR *entries = opendir(directory);
    struct dirent *entry;
    if (entries == NULL) return;
    while ((entry = readdir(entries)) != NULL) {
        const char *name = entry->d_name;
        char *end;
        long raw;
        if (name[0] == 'b' || name[0] == 'x') ++name;
        if (name[0] < '1' || name[0] > '9') continue;
        errno = 0;
        raw = strtol(name, &end, 10);
        if (errno == 0 && *end == 0 && raw > 0 && raw <= INT32_MAX) domain_record_remove(directory, (pid_t)raw);
    }
    (void)closedir(entries);
    (void)rmdir(directory);
}

static void domain_network_remove(hl_process_domain domain) {
    char directory[96];
    DIR *entries;
    struct dirent *entry;
    snprintf(directory, sizeof directory, "/tmp/.hl-net-%016llx%016llx", (unsigned long long)domain.identity[0],
             (unsigned long long)domain.identity[1]);
    entries = opendir(directory);
    if (entries == NULL) return;
    while ((entry = readdir(entries)) != NULL) {
        const char *name = entry->d_name;
        if (name[0] != 'p' || name[1] < '0' || name[1] > '9') continue;
        for (const char *digit = name + 1; *digit != 0; ++digit)
            if (*digit < '0' || *digit > '9') goto next;
        (void)unlinkat(dirfd(entries), name, 0);
    next:;
    }
    (void)closedir(entries);
    (void)rmdir(directory);
}
#endif /* !_WIN32 */

static int process_info_compare(const void *left, const void *right) {
    const hl_activation_process_info *a = left;
    const hl_activation_process_info *b = right;
    return a->host_id < b->host_id ? -1 : a->host_id > b->host_id ? 1 : 0;
}

#if defined(_WIN32)

hl_status hl_activation_domain_processes(hl_process_domain domain, uint64_t initial_process_id,
                                         hl_activation_process_info *processes, uint32_t capacity,
                                         uint32_t *out_count) {
    HANDLE job;
    DWORD *ids = NULL;
    uint32_t total = 0;
    uint32_t index;
    if ((domain.identity[0] | domain.identity[1]) == 0 || initial_process_id == 0 || out_count == NULL ||
        (capacity != 0 && processes == NULL))
        return HL_STATUS_INVALID_ARGUMENT;
    job = domain_job_open(domain, 0);
    if (job == NULL) {
        /* No such job is the ENOENT case: a domain nothing ever joined, or one
         * whose last member exited and whose creator released it. Zero members,
         * not a failure -- the POSIX arm answers the same way for a missing
         * registry directory. */
        *out_count = 0;
        return HL_STATUS_OK;
    }
    if (capacity != 0) {
        ids = calloc(capacity, sizeof(*ids));
        if (ids == NULL) {
            CloseHandle(job);
            return HL_STATUS_OUT_OF_MEMORY;
        }
    }
    if (domain_job_processes(job, ids, capacity, &total) != 0) {
        free(ids);
        CloseHandle(job);
        return HL_STATUS_PLATFORM_FAILURE;
    }
    CloseHandle(job);
    for (index = 0; index < total && index < capacity; ++index) {
        processes[index].host_id = (uint64_t)ids[index];
        /* There is no separate supervisor process on this host: the activation
         * child IS the initial guest process, and every later guest process is
         * its descendant. So the identity equality the POSIX arm keeps for
         * direct launches is the only case here, and the parent-pid test it
         * adds for the supervisor case has nothing to match. */
        processes[index].initial = (uint64_t)ids[index] == initial_process_id ? 1u : 0u;
        processes[index].reserved = 0;
    }
    free(ids);
    *out_count = total;
    if (total > capacity) return HL_STATUS_RESOURCE_LIMIT;
    if (total > 1) qsort(processes, total, sizeof(*processes), process_info_compare);
    return HL_STATUS_OK;
}

hl_status hl_activation_domain_terminate(hl_process_domain domain) {
    HANDLE job;
    unsigned round;
    unsigned empty = 0;
    if ((domain.identity[0] | domain.identity[1]) == 0) return HL_STATUS_INVALID_ARGUMENT;
    job = domain_job_open(domain, 0);
    if (job == NULL) return HL_STATUS_OK; /* Repeated termination is successful. */
    /* Same two-clean-rounds shape as the POSIX arm, and for the same reason: a
     * member may be mid-spawn when the sweep runs, so one empty observation is
     * not proof the domain is drained. TerminateJobObject is atomic over the
     * whole membership, which is the part that needs no retry; the retry is for
     * processes that joined between two of them. */
    for (round = 0; round < 200u; ++round) {
        uint32_t live = 0;
        if (!TerminateJobObject(job, HL_ACTIVATION_WINDOWS_KILL_CODE)) {
            CloseHandle(job);
            return HL_STATUS_PLATFORM_FAILURE;
        }
        if (domain_job_processes(job, NULL, 0, &live) != 0) {
            CloseHandle(job);
            return HL_STATUS_PLATFORM_FAILURE;
        }
        if (live == 0) {
            if (++empty >= 2u) {
                CloseHandle(job);
                return HL_STATUS_OK;
            }
        } else {
            empty = 0;
        }
        (void)poll(NULL, 0, 10);
    }
    CloseHandle(job);
    return HL_STATUS_BUSY;
}

#else

hl_status hl_activation_domain_processes(hl_process_domain domain, uint64_t initial_process_id,
                                         hl_activation_process_info *processes, uint32_t capacity,
                                         uint32_t *out_count) {
    char directory[96];
    DIR *entries;
    struct dirent *entry;
    uint32_t count = 0;
    if ((domain.identity[0] | domain.identity[1]) == 0 || initial_process_id == 0 || out_count == NULL ||
        (capacity != 0 && processes == NULL))
        return HL_STATUS_INVALID_ARGUMENT;
    domain_path(domain, directory, sizeof directory);
    entries = opendir(directory);
    if (entries == NULL) {
        if (errno != ENOENT) return HL_STATUS_PLATFORM_FAILURE;
        *out_count = 0;
        return HL_STATUS_OK;
    }
    while ((entry = readdir(entries)) != NULL) {
        char *end;
        long raw;
        uint64_t expected;
        hl_host_process_info process;
        if (entry->d_name[0] != 'b' || entry->d_name[1] < '1' || entry->d_name[1] > '9') continue;
        errno = 0;
        raw = strtol(entry->d_name + 1, &end, 10);
        if (errno != 0 || *end != 0 || raw <= 0 || raw > INT32_MAX) continue;
        if (!domain_birth(directory, (pid_t)raw, &expected) || !hl_host_process_read(raw, &process) ||
            process.start_time_ns != expected) {
            /* Publication writes the membership and birth records in separate
             * steps. A snapshot racing that sequence must skip the incomplete
             * member without revoking it; a later snapshot will validate it.
             * Domain teardown owns stale-record reclamation. */
            continue;
        }
        if (count < capacity) {
            processes[count].host_id = (uint64_t)raw;
            /* The opaque activation handle owns a short-lived supervisor. The
             * initial guest process is its direct child; later guest forks are
             * descendants of that child. Direct launches that do not need the
             * supervisor retain the identity equality case. */
            processes[count].initial =
                (uint64_t)raw == initial_process_id || (uint64_t)process.parent_pid == initial_process_id ? 1u : 0u;
            processes[count].reserved = 0;
        }
        if (count == UINT32_MAX) {
            (void)closedir(entries);
            return HL_STATUS_RESOURCE_LIMIT;
        }
        ++count;
    }
    (void)closedir(entries);
    *out_count = count;
    if (count > capacity) return HL_STATUS_RESOURCE_LIMIT;
    if (count > 1) qsort(processes, count, sizeof(*processes), process_info_compare);
    return HL_STATUS_OK;
}

hl_status hl_activation_domain_terminate(hl_process_domain domain) {
    char directory[96];
    unsigned round;
    unsigned empty = 0;
    if ((domain.identity[0] | domain.identity[1]) == 0) return HL_STATUS_INVALID_ARGUMENT;
    domain_path(domain, directory, sizeof directory);
    for (round = 0; round < 200; ++round) {
        DIR *entries = opendir(directory);
        struct dirent *entry;
        unsigned live = 0;
        if (entries == NULL) {
            if (errno != ENOENT) return HL_STATUS_PLATFORM_FAILURE;
            domain_network_remove(domain);
            return HL_STATUS_OK;
        }
        while ((entry = readdir(entries)) != NULL) {
            char *end;
            long raw;
            uint64_t expected;
            hl_host_process_info process;
            if (entry->d_name[0] != 'b' || entry->d_name[1] < '1' || entry->d_name[1] > '9') continue;
            errno = 0;
            raw = strtol(entry->d_name + 1, &end, 10);
            if (errno != 0 || *end != 0 || raw <= 0 || raw > INT32_MAX) continue;
            if (!domain_birth(directory, (pid_t)raw, &expected) || !hl_host_process_read(raw, &process) ||
                process.start_time_ns != expected) {
                domain_record_remove(directory, (pid_t)raw);
                continue;
            }
            ++live;
            if (kill((pid_t)raw, SIGKILL) != 0 && errno != ESRCH) {
                (void)closedir(entries);
                return HL_STATUS_PLATFORM_FAILURE;
            }
        }
        (void)closedir(entries);
        if (live == 0) {
            if (++empty >= 2) {
                domain_directory_remove(directory);
                domain_network_remove(domain);
                return HL_STATUS_OK;
            }
        } else {
            empty = 0;
        }
        (void)poll(NULL, 0, 10);
    }
    return HL_STATUS_BUSY;
}

#endif /* _WIN32 */

#if !defined(_WIN32)
static int wait_child(pid_t child, int *waited) {
    pid_t result;
    do {
        result = waitpid(child, waited, 0);
    } while (result < 0 && errno == EINTR);
    return result == child ? 0 : -1;
}
#endif

#if !defined(_WIN32)
/* A terminal master is reported to the caller, and hl_activation_descriptor spells "no terminal" as
 * HL_ACTIVATION_DESCRIPTOR_NONE == 0. openpty() hands back the lowest free descriptor, which is 0 whenever
 * the embedder closed its standard input -- a live master indistinguishable from "absent". Move it off
 * zero at the source so no caller of either descriptor form can be handed that value. Returns the
 * (possibly new) descriptor, or -1 after closing the original when it cannot be moved. */
static int reserve_master_descriptor(int master) {
    int moved;
    if (master != 0) return master;
    moved = fcntl(master, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    (void)close(master);
    return moved;
}

static int reserve_control_descriptors(int pair[2]) {
    size_t index;
    for (index = 0; index < 2; ++index) {
        int replacement;
        if (pair[index] > STDERR_FILENO) continue;
        replacement = fcntl(pair[index], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (replacement < 0) return -1;
        (void)close(pair[index]);
        pair[index] = replacement;
    }
    return 0;
}
#endif /* !_WIN32 */

static void cache_failure(hl_activation_process *process, hl_status status) {
    process->finished = 1;
    process->final_status = status;
    process->final_exit = (hl_engine_exit){.abi = HL_ENGINE_ABI,
                                           .size = sizeof(process->final_exit),
                                           .kind = HL_ENGINE_EXIT_ENGINE_ERROR,
                                           .detail = (uint64_t)status};
}

/*
 * Parent-side preflight, shared by both hosts because none of it is host work:
 * it validates the caller's arguments and fills in the request bytes the child
 * will validate again on arrival. It also CONSUMES the one-shot test mode, which
 * is why it must run exactly once per start on either arm.
 *
 * The descriptor arguments are still POSIX-shaped ints here (-1 absent) because
 * that is the internal spelling everything below this line uses; on Windows the
 * value in one is a handle narrowed to 32 bits, which is the width the public
 * hl_activation_descriptor contract already pins.
 */
static hl_status activation_prepare(const char *executable, uint32_t guest_isa, const char *guest,
                                    const hl_activation_stdio *stdio, const hl_terminal_size *terminal,
                                    int32_t *out_master, int transport, int checkpoint, int trigger,
                                    hl_activation_process **out_process, hl_activation_request *request,
                                    uint32_t *out_test_mode) {
    size_t path_size;
    uint32_t test_mode;
    if (out_process == NULL) return HL_STATUS_INVALID_ARGUMENT;
    *out_process = NULL;
    if (out_master != NULL) *out_master = -1;
    if (executable == NULL || guest == NULL || !activation_absolute(executable) || !activation_absolute(guest) ||
        (guest_isa != HL_GUEST_ISA_AARCH64 && guest_isa != HL_GUEST_ISA_X86_64))
        return HL_STATUS_INVALID_ARGUMENT;
    if (stdio != NULL && (stdio->input < -1 || stdio->output < -1 || stdio->error < -1))
        return HL_STATUS_INVALID_ARGUMENT;
    if (transport < -1 || checkpoint < -1 || trigger < -1) return HL_STATUS_INVALID_ARGUMENT;
    if (terminal != NULL && (stdio != NULL || out_master == NULL || terminal->rows == 0 || terminal->columns == 0))
        return HL_STATUS_INVALID_ARGUMENT;
    path_size = strlen(guest) + 1;
    if (path_size > sizeof(request->path)) return HL_STATUS_INVALID_ARGUMENT;
    memset(request, 0, sizeof(*request));
    request->magic = HL_ACTIVATION_MAGIC;
    request->abi = HL_ACTIVATION_ABI;
    request->size = sizeof(*request);
    request->guest_isa = guest_isa;
    request->path_size = (uint32_t)path_size;
    request->descriptor_roles = (transport >= 0 ? (uint32_t)HL_ACTIVATION_ROLE_TRANSPORT : 0u) |
                                (checkpoint >= 0 ? (uint32_t)HL_ACTIVATION_ROLE_CHECKPOINT : 0u) |
                                (trigger >= 0 ? (uint32_t)HL_ACTIVATION_ROLE_TRIGGER : 0u);
    request->reserved = (transport >= 0 ? 1u : 0u) + (checkpoint >= 0 ? 1u : 0u) + (trigger >= 0 ? 1u : 0u);
    test_mode = activation_test_mode;
    request->test_flags = test_mode == 1 ? 1u : test_mode == 4 ? 4u : test_mode == 5 ? 5u : 0u;
    if (test_mode == 2) request->magic ^= UINT64_C(1);
    activation_test_mode = 0;
    memcpy(request->path, guest, path_size);
    arc4random_buf(request->nonce, sizeof(request->nonce));
    *out_test_mode = test_mode;
    return HL_STATUS_OK;
}

/*
 * Parent side of the handshake, from "a child exists and the control channel is
 * connected" to "the child is committed and running the guest". Shared: it is
 * the protocol, and the protocol is the same on both hosts down to the byte.
 * The only host-shaped step is the half-close the truncation test needs, which
 * has no meaning on a channel that is not a socket.
 *
 * Returns 0 on success. On failure the caller owns the teardown, because what a
 * half-started child needs killed differs between the two arms.
 */
static int activation_handshake(int control, const hl_activation_request *request, uint32_t test_mode,
                                const int *attached, int attached_count) {
    hl_activation_reply reply;
    unsigned char commit = 0xa5u;
    size_t size = test_mode == 3 ? sizeof(*request) / 2u : sizeof(*request);
#if defined(_WIN32)
    /* The attachment already happened, at spawn: the handles are in the child's
     * inheritance set and their values are inside the request. So the send is a
     * plain write of the same bytes the POSIX arm sends alongside its ancillary
     * data, and attached/attached_count are consumed by the caller rather than
     * here. */
    (void)attached;
    (void)attached_count;
    if (transfer(control, (void *)(uintptr_t)request, size, 1) != 0) return -1;
    /* The truncation test's POSIX half-close has no counterpart on a duplex pipe:
     * shutdown(SHUT_WR) is what lets the child's read return 0 and exit, and
     * without it both sides would block forever -- the child waiting for the rest
     * of a request that is not coming, the parent waiting for a reply that will
     * never be sent. Failing here reaches the same observable end state (start
     * reports CORRUPT and the caller tears the child down through its job) by the
     * only route this transport allows. */
    if (test_mode == 3) return -1;
#else
    if ((attached_count > 0
             ? hl_fork_wire_send_descriptors(control, request, sizeof(*request), attached, attached_count)
             : hl_fork_wire_send_descriptors(control, request, size, NULL, 0)) != 0)
        return -1;
    if (test_mode == 3) (void)shutdown(control, SHUT_WR);
#endif
    if (transfer(control, &reply, sizeof(reply), 0) != 0) return -1;
    if (reply.magic != request->magic || reply.abi != request->abi || reply.size != sizeof(reply) ||
        memcmp(reply.nonce, request->nonce, sizeof(request->nonce)) != 0 || reply.status != HL_STATUS_OK)
        return -1;
    return transfer(control, &commit, 1, 1);
}

#if !defined(_WIN32)

static hl_status activation_start(const char *executable, uint32_t guest_isa, const char *guest,
                                  const hl_activation_stdio *stdio, const hl_terminal_size *terminal,
                                  int32_t *out_master, int transport, int checkpoint, int trigger,
                                  hl_activation_process **out_process) {
    int pair[2];
    pid_t child;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    hl_activation_request request;
    char activation[] = "HL_ACTIVATION_FD=3";
    char *child_argv[2];
    char **child_env;
    size_t env_count = 0;
    size_t env_output = 0;
    int waited = 0;
    uint32_t test_mode;
    hl_status prepared;
    hl_activation_process *process;
    int master = -1;
    int slave = -1;
    prepared = activation_prepare(executable, guest_isa, guest, stdio, terminal, out_master, transport, checkpoint,
                                  trigger, out_process, &request, &test_mode);
    if (prepared != HL_STATUS_OK) return prepared;
#if defined(__APPLE__)
    hl_linux_dns_prepare();
#endif
    while (environ[env_count] != NULL)
        ++env_count;
    child_env = calloc(env_count + 2, sizeof(*child_env));
    if (child_env == NULL) return HL_STATUS_OUT_OF_MEMORY;
    for (env_count = 0; environ[env_count] != NULL; ++env_count)
        if (strncmp(environ[env_count], "HL_ACTIVATION_FD=", 17) != 0) child_env[env_output++] = environ[env_count];
    child_env[env_output] = activation;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        free(child_env);
        return HL_STATUS_PLATFORM_FAILURE;
    }
    if (reserve_control_descriptors(pair) != 0) {
        close(pair[0]);
        close(pair[1]);
        free(child_env);
        return HL_STATUS_PLATFORM_FAILURE;
    }
#if defined(__APPLE__)
    {
        int enabled = 1;
        if (setsockopt(pair[0], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0 ||
            setsockopt(pair[1], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
            close(pair[0]);
            close(pair[1]);
            free(child_env);
            return HL_STATUS_PLATFORM_FAILURE;
        }
    }
#endif
    if (fcntl(pair[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(pair[1], F_SETFD, FD_CLOEXEC) != 0) {
        close(pair[0]);
        close(pair[1]);
        free(child_env);
        return HL_STATUS_PLATFORM_FAILURE;
    }
    child_argv[0] = (char *)(uintptr_t)executable;
    child_argv[1] = NULL;
    if (terminal != NULL) {
        struct winsize size = {.ws_row = terminal->rows, .ws_col = terminal->columns};
#if !defined(__linux__)
        int close_limit = getdtablesize();
#endif
        int opened = openpty(&master, &slave, NULL, NULL, &size);
        if (opened == 0) master = reserve_master_descriptor(master);
        if (opened != 0 || master < 0 || fcntl(master, F_SETFD, FD_CLOEXEC) != 0 ||
            fcntl(slave, F_SETFD, FD_CLOEXEC) != 0) {
            if (master >= 0) close(master);
            if (slave >= 0) close(slave);
            close(pair[0]);
            close(pair[1]);
            free(child_env);
            return HL_STATUS_PLATFORM_FAILURE;
        }
        child = fork();
        if (child == 0) {
            (void)close(master);
            (void)close(pair[0]);
            if (setsid() < 0 || ioctl(slave, TIOCSCTTY, 0) < 0 || dup2(slave, 0) < 0 || dup2(slave, 1) < 0 ||
                dup2(slave, 2) < 0 || dup2(pair[1], HL_ACTIVATION_FD) < 0)
                _exit(126);
            /*
             * execve preserves every descriptor without FD_CLOEXEC.  The
             * embedder may be a large process with sockets and event queues
             * that have nothing to do with this launch; none may become guest
             * state.  Descriptor 3 is the sole activation capability and
             * closefrom removes everything above it atomically with respect
             * to this already-forked child.
             */
#if defined(__linux__)
            closefrom(HL_ACTIVATION_FD + 1);
#else
            for (int fd = HL_ACTIVATION_FD + 1; fd < close_limit; ++fd)
                (void)close(fd);
#endif
            execve(executable, child_argv, child_env);
            _exit(127);
        }
        (void)close(slave);
        slave = -1;
        if (child < 0) {
            close(master);
            close(pair[0]);
            close(pair[1]);
            free(child_env);
            return HL_STATUS_PLATFORM_FAILURE;
        }
        goto spawned;
    }
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(pair[0]);
        close(pair[1]);
        free(child_env);
        return HL_STATUS_PLATFORM_FAILURE;
    }
    if (posix_spawnattr_init(&attributes) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        close(pair[0]);
        close(pair[1]);
        free(child_env);
        return HL_STATUS_PLATFORM_FAILURE;
    }
    short spawn_flags = POSIX_SPAWN_SETPGROUP;
#if defined(__APPLE__)
    /*
     * Darwin's CLOEXEC_DEFAULT is the race-free form of descriptor isolation:
     * only descriptors named by file actions survive.  Explicitly inherit
     * untouched standard streams; dup2 actions already name redirected ones.
     */
    spawn_flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
    if (((stdio == NULL || stdio->input < 0) && posix_spawn_file_actions_addinherit_np(&actions, 0) != 0) ||
        ((stdio == NULL || stdio->output < 0) && posix_spawn_file_actions_addinherit_np(&actions, 1) != 0) ||
        ((stdio == NULL || stdio->error < 0) && posix_spawn_file_actions_addinherit_np(&actions, 2) != 0)) {
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        close(pair[0]);
        close(pair[1]);
        free(child_env);
        return HL_STATUS_PLATFORM_FAILURE;
    }
#endif
    if (posix_spawnattr_setflags(&attributes, spawn_flags) != 0 || posix_spawnattr_setpgroup(&attributes, 0) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        close(pair[0]);
        close(pair[1]);
        free(child_env);
        return HL_STATUS_PLATFORM_FAILURE;
    }
    if (stdio != NULL && ((stdio->input >= 0 && posix_spawn_file_actions_adddup2(&actions, stdio->input, 0) != 0) ||
                          (stdio->output >= 0 && posix_spawn_file_actions_adddup2(&actions, stdio->output, 1) != 0) ||
                          (stdio->error >= 0 && posix_spawn_file_actions_adddup2(&actions, stdio->error, 2) != 0))) {
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        close(pair[0]);
        close(pair[1]);
        free(child_env);
        return HL_STATUS_PLATFORM_FAILURE;
    }
    /*
     * Close the parent endpoint before installing descriptor 3: pair[0] is
     * commonly descriptor 3 itself.  On Linux, closefrom_np complements the
     * Darwin default by closing every descriptor above the activation slot
     * after all stdio/control dup2 actions have consumed their sources.
     */
    if (posix_spawn_file_actions_addclose(&actions, pair[0]) != 0 ||
        posix_spawn_file_actions_adddup2(&actions, pair[1], HL_ACTIVATION_FD) != 0 ||
        (pair[1] != HL_ACTIVATION_FD && posix_spawn_file_actions_addclose(&actions, pair[1]) != 0)
#if defined(__linux__)
        || posix_spawn_file_actions_addclosefrom_np(&actions, HL_ACTIVATION_FD + 1) != 0
#endif
    ) {
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        close(pair[0]);
        close(pair[1]);
        free(child_env);
        return HL_STATUS_PLATFORM_FAILURE;
    }
    if (posix_spawn(&child, executable, &actions, &attributes, child_argv, child_env) != 0) {
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        close(pair[0]);
        close(pair[1]);
        free(child_env);
        return HL_STATUS_PLATFORM_FAILURE;
    }
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
spawned:
    close(pair[1]);
    free(child_env);
    {
        int attached[3];
        int attached_count = 0;
        /* Ascending role-bit order, matching hl_activation_child's re-seating. */
        if (transport >= 0) attached[attached_count++] = transport;
        if (checkpoint >= 0) attached[attached_count++] = checkpoint;
        if (trigger >= 0) attached[attached_count++] = trigger;
        if (test_mode == 3) attached_count = 0;
        if (activation_handshake(pair[0], &request, test_mode, attached, attached_count) != 0) {
            close(pair[0]);
            if (master >= 0) close(master);
            (void)kill(-child, SIGKILL);
            (void)wait_child(child, &waited);
            return HL_STATUS_CORRUPT;
        }
    }
    process = calloc(1, sizeof(*process));
    if (process == NULL) {
        close(pair[0]);
        if (master >= 0) close(master);
        (void)kill(-child, SIGKILL);
        wait_child(child, &waited);
        return HL_STATUS_OUT_OF_MEMORY;
    }
    process->descriptor = pair[0];
    process->pid = child;
    memcpy(process->nonce, request.nonce, sizeof(process->nonce));
    if (out_master != NULL) *out_master = master;
    *out_process = process;
    return HL_STATUS_OK;
}

#else /* Windows */

/* UTF-8 to UTF-16 for the three strings CreateProcessW takes. The host string
 * policy above this layer is UTF-8, so this is a decode and not a guess; a byte
 * sequence that is not valid UTF-8 is refused rather than replaced, because the
 * result would name a different file. Caller frees. */
static WCHAR *activation_widen(const char *text) {
    int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    WCHAR *wide;
    if (units <= 0) return NULL;
    wide = calloc((size_t)units, sizeof(*wide));
    if (wide == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, units) != units) {
        free(wide);
        return NULL;
    }
    return wide;
}

/* The same string as one quoted command-line argument. A path may contain
 * spaces; it may not contain a double quote, which is not a legal character in a
 * Windows path, so wrapping is sufficient and no escaping rule is needed.
 * Caller frees. */
static WCHAR *activation_quoted(const char *text) {
    WCHAR *inner = activation_widen(text);
    size_t length;
    WCHAR *quoted;
    if (inner == NULL) return NULL;
    length = wcslen(inner);
    quoted = calloc(length + 3u, sizeof(*quoted));
    if (quoted == NULL) {
        free(inner);
        return NULL;
    }
    quoted[0] = L'"';
    memcpy(quoted + 1, inner, length * sizeof(*quoted));
    quoted[length + 1] = L'"';
    free(inner);
    return quoted;
}

/*
 * The child's environment block: this process's, minus any inherited
 * HL_ACTIVATION_FD, plus the one this launch is publishing. Built explicitly
 * rather than inherited-and-patched because there is no per-child SetEnvironment:
 * the parent's own environment is the only thing CreateProcessW would otherwise
 * pass, and mutating that to spawn a child would be visible to every other
 * thread in the embedder. Caller frees.
 */
static WCHAR *activation_child_environment(unsigned long long handle_value) {
    WCHAR *block = GetEnvironmentStringsW();
    WCHAR addition[48];
    size_t addition_length;
    size_t kept = 0;
    WCHAR *built;
    const WCHAR *scan;
    size_t offset = 0;
    if (block == NULL) return NULL;
    addition_length =
        (size_t)swprintf(addition, sizeof addition / sizeof addition[0], L"HL_ACTIVATION_FD=%llu", handle_value);
    for (scan = block; *scan != L'\0'; scan += wcslen(scan) + 1)
        if (_wcsnicmp(scan, L"HL_ACTIVATION_FD=", 17) != 0) kept += wcslen(scan) + 1;
    built = calloc(kept + addition_length + 2u, sizeof(*built));
    if (built == NULL) {
        (void)FreeEnvironmentStringsW(block);
        return NULL;
    }
    for (scan = block; *scan != L'\0'; scan += wcslen(scan) + 1) {
        size_t length = wcslen(scan) + 1;
        if (_wcsnicmp(scan, L"HL_ACTIVATION_FD=", 17) == 0) continue;
        memcpy(built + offset, scan, length * sizeof(*built));
        offset += length;
    }
    memcpy(built + offset, addition, (addition_length + 1) * sizeof(*built));
    /* offset + addition_length + 1 is the second NUL that ends the block; calloc
     * already zeroed it. */
    (void)FreeEnvironmentStringsW(block);
    return built;
}

/*
 * The control channel: one duplex named pipe, standing in for the AF_UNIX
 * socketpair.
 *
 * A pair of anonymous pipes would also work and would need no namespace, but it
 * would need TWO handle values in the child and the activation environment
 * variable carries one. A duplex pipe keeps that contract, and keeps every
 * caller of transfer() -- both directions, both processes -- pointed at a single
 * descriptor, which is what lets the child body below be the same code as the
 * POSIX one.
 *
 * The name is not a secret and is not relied on as one. FILE_FLAG_FIRST_PIPE_INSTANCE
 * makes a pre-existing squatter a hard failure of THIS call rather than a silent
 * connection to someone else's pipe; nMaxInstances 1 means no second server can
 * appear behind us; PIPE_REJECT_REMOTE_CLIENTS keeps the reach local. Beyond
 * that, the protocol's own 128-bit nonce is what makes a hijacked channel
 * useless: a connector that cannot echo it never gets the commit byte, and the
 * parent tears the child down.
 */
static int activation_control_pair(HANDLE *out_server, HANDLE *out_client) {
    SECURITY_ATTRIBUTES inheritable = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
    char name[96];
    uint64_t unique[2];
    HANDLE server;
    HANDLE client;
    arc4random_buf(unique, sizeof unique);
    snprintf(name, sizeof name, "\\\\.\\pipe\\hl-activation.%08lx.%016llx%016llx", GetCurrentProcessId(),
             (unsigned long long)unique[0], (unsigned long long)unique[1]);
    server = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536,
                              65536, 0, NULL);
    if (server == INVALID_HANDLE_VALUE) return -1;
    client = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, &inheritable, OPEN_EXISTING, 0, NULL);
    if (client == INVALID_HANDLE_VALUE) {
        CloseHandle(server);
        return -1;
    }
    /* The client connected before this call, which is the documented
     * ERROR_PIPE_CONNECTED case and not a failure. */
    if (!ConnectNamedPipe(server, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
        CloseHandle(client);
        CloseHandle(server);
        return -1;
    }
    *out_server = server;
    *out_client = client;
    return 0;
}

/* An inheritable duplicate of a borrowed handle. The original is untouched --
 * the caller may close it the moment start returns, which is the ownership rule
 * the public header states -- and SetHandleInformation is deliberately not used
 * instead: flipping the inherit flag on a handle the embedder also owns would
 * change what EVERY other CreateProcess in the embedder inherits. */
static HANDLE activation_inheritable(HANDLE original) {
    HANDLE copy = NULL;
    HANDLE self = GetCurrentProcess();
    if (!DuplicateHandle(self, original, self, &copy, 0, TRUE, DUPLICATE_SAME_ACCESS)) return NULL;
    return copy;
}

/* The domain a launch belongs to, read out of the ABI5 wire file the caller
 * named. The child parses the same file for everything else, but the job
 * assignment has to happen while the child is still suspended and before it is
 * put in this activation's own job -- a job can only nest INSIDE one that
 * already contains it -- so the parent needs this one field early. A file that
 * does not validate is not an error here: the child will reject it with a real
 * status, and reporting a second opinion from the parent would just make the
 * failure harder to read. Returns 1 when a domain was found. */
static int activation_config_domain(const char *path, hl_process_domain *out_domain) {
    hl_launch_config config;
    unsigned char *wire;
    long size;
    int found = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 0;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0 || (size_t)size < sizeof(config) ||
        fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        return 0;
    }
    wire = malloc((size_t)size);
    if (wire == NULL) {
        (void)fclose(file);
        return 0;
    }
    if (fread(wire, 1, (size_t)size, file) == (size_t)size &&
        hl_launch_config_validate(wire, (size_t)size, &config, NULL) == HL_STATUS_OK) {
        out_domain->identity[0] = config.process_domain[0];
        out_domain->identity[1] = config.process_domain[1];
        found = (out_domain->identity[0] | out_domain->identity[1]) != 0;
    }
    free(wire);
    (void)fclose(file);
    return found;
}

static hl_status activation_start(const char *executable, uint32_t guest_isa, const char *guest,
                                  const hl_activation_stdio *stdio, const hl_terminal_size *terminal,
                                  int32_t *out_master, int transport, int checkpoint, int trigger,
                                  hl_activation_process **out_process) {
    hl_activation_request request;
    hl_activation_process *process;
    hl_process_domain domain;
    STARTUPINFOEXW startup;
    PROCESS_INFORMATION spawned;
    SIZE_T attribute_size = 0;
    /* control client, transport, checkpoint, trigger, three streams. */
    HANDLE inherited[7];
    uint32_t inherited_count = 0;
    HANDLE server = NULL;
    HANDLE client = NULL;
    HANDLE job = NULL;
    HANDLE domain_job = NULL;
    WCHAR *image = NULL;
    WCHAR *command = NULL;
    WCHAR *environment = NULL;
    int control = -1;
    uint32_t test_mode;
    hl_status prepared;
    hl_status status = HL_STATUS_PLATFORM_FAILURE;
    unsigned index;
    prepared = activation_prepare(executable, guest_isa, guest, stdio, terminal, out_master, transport, checkpoint,
                                  trigger, out_process, &request, &test_mode);
    if (prepared != HL_STATUS_OK) return prepared;
    if (terminal != NULL) {
        /* Named at the entry rather than deep inside a spawn that would half
         * succeed: this host has no object that is at once the child's
         * controlling terminal and one descriptor the parent reads and writes.
         * See the file header. */
        return HL_STATUS_NOT_SUPPORTED;
    }
    memset(&startup, 0, sizeof(startup));
    memset(&spawned, 0, sizeof(spawned));
    startup.StartupInfo.cb = sizeof(startup);

    if (activation_control_pair(&server, &client) != 0) return HL_STATUS_PLATFORM_FAILURE;
    inherited[inherited_count++] = client;

    /* SCM_RIGHTS, in two halves: the handle joins the child's inheritance set
     * (which fixes its value there), and that value goes in the request. Order
     * is the ascending role-bit order hl_activation_child re-seats by. */
    {
        const int borrowed[3] = {transport, checkpoint, trigger};
        uint32_t attached = 0;
        for (index = 0; index < 3u; ++index) {
            HANDLE copy;
            if (borrowed[index] < 0) continue;
            copy = activation_inheritable((HANDLE)(intptr_t)borrowed[index]);
            if (copy == NULL) goto fail;
            inherited[inherited_count++] = copy;
            request.handles[attached++] = (uint64_t)(uintptr_t)copy;
        }
        /* The truncation test withholds the descriptors as well as the tail of
         * the request, exactly as the POSIX arm does. */
        if (test_mode == 3) {
            for (index = 0; index < attached; ++index)
                request.handles[index] = 0;
            request.reserved = 0;
            request.descriptor_roles = 0;
        }
    }

    /* Standard streams. STARTF_USESTDHANDLES is all-or-nothing, so an unredirected
     * stream is explicitly this process's own -- the same "inherit the
     * application's stream" the absent descriptor means everywhere in this API. */
    {
        const int requested[3] = {stdio != NULL ? stdio->input : -1, stdio != NULL ? stdio->output : -1,
                                  stdio != NULL ? stdio->error : -1};
        const DWORD standard[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
        HANDLE *targets[3] = {&startup.StartupInfo.hStdInput, &startup.StartupInfo.hStdOutput,
                              &startup.StartupInfo.hStdError};
        for (index = 0; index < 3u; ++index) {
            HANDLE source = requested[index] >= 0 ? (HANDLE)(intptr_t)requested[index] : GetStdHandle(standard[index]);
            HANDLE copy;
            if (source == NULL || source == INVALID_HANDLE_VALUE) {
                /* A stream this process does not have. NULL is what a child
                 * inherits for a stream nobody supplied, and it is not a
                 * handle, so it must not enter the inheritance list. */
                *targets[index] = NULL;
                continue;
            }
            copy = activation_inheritable(source);
            if (copy == NULL) goto fail;
            inherited[inherited_count++] = copy;
            *targets[index] = copy;
        }
        startup.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
    }

    image = activation_widen(executable);
    /* The command line is separate from the image path and is QUOTED, because the
     * two are parsed by different rules: lpApplicationName is taken literally,
     * lpCommandLine is split on spaces to build the child's argv. The POSIX arm
     * passes the executable as one argv element and this has to mean the same
     * thing, which an unquoted "C:\Program Files\..." would not. */
    command = activation_quoted(executable);
    environment = activation_child_environment((unsigned long long)(uintptr_t)client);
    if (image == NULL || command == NULL || environment == NULL) {
        status = HL_STATUS_OUT_OF_MEMORY;
        goto fail;
    }

    /* bInheritHandles alone would inherit every inheritable handle in the
     * embedder; the attribute list alone does nothing. Together they mean
     * "these and only these", which is this host's closefrom -- enforced by the
     * kernel at creation rather than swept afterwards. */
    if (InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size) || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        goto fail;
    startup.lpAttributeList = malloc(attribute_size);
    if (startup.lpAttributeList == NULL) {
        status = HL_STATUS_OUT_OF_MEMORY;
        goto fail;
    }
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size)) {
        free(startup.lpAttributeList);
        startup.lpAttributeList = NULL;
        goto fail;
    }
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                   (SIZE_T)inherited_count * sizeof(inherited[0]), NULL, NULL))
        goto fail;

    job = CreateJobObjectW(NULL, NULL);
    if (job == NULL) goto fail;
    if (activation_config_domain(guest, &domain)) domain_job = domain_job_open(domain, 1);

    /* CREATE_SUSPENDED so both job assignments land before the child runs a
     * single instruction: a process that started and forked first would leave
     * the fork outside the job. CREATE_NEW_PROCESS_GROUP mirrors the POSIX arm's
     * setpgroup -- a console interrupt aimed at the embedder must not also land
     * on the guest, and the child restores its own Ctrl+C disposition. */
    if (!CreateProcessW(image, command, NULL, NULL, TRUE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP |
                            EXTENDED_STARTUPINFO_PRESENT,
                        environment, NULL, &startup.StartupInfo, &spawned))
        goto fail;
    /* Domain job FIRST. Jobs nest one way only: this activation's job may become
     * a child of the domain job, but not the reverse, and a domain shared by two
     * activations could not nest inside either one's. Assigning the outer one
     * first is what makes both memberships legal. A domain assignment that fails
     * is not fatal -- the launch still runs, and domain_processes will simply not
     * see it -- but the activation job is, because kill depends on it. */
    if (domain_job != NULL && !AssignProcessToJobObject(domain_job, spawned.hProcess)) {
        CloseHandle(domain_job);
        domain_job = NULL;
    }
    if (!AssignProcessToJobObject(job, spawned.hProcess)) {
        (void)TerminateProcess(spawned.hProcess, HL_ACTIVATION_WINDOWS_KILL_CODE);
        CloseHandle(spawned.hThread);
        CloseHandle(spawned.hProcess);
        memset(&spawned, 0, sizeof(spawned));
        goto fail;
    }
    if (ResumeThread(spawned.hThread) == (DWORD)-1) {
        (void)TerminateProcess(spawned.hProcess, HL_ACTIVATION_WINDOWS_KILL_CODE);
        CloseHandle(spawned.hThread);
        CloseHandle(spawned.hProcess);
        memset(&spawned, 0, sizeof(spawned));
        goto fail;
    }
    CloseHandle(spawned.hThread);
    spawned.hThread = NULL;

    /* The child owns its own copies now. Releasing the parent's duplicates here
     * -- including the control client -- is what makes a child exit break the
     * pipe, which is the hang-up try_wait and wait both read. */
    for (index = 0; index < inherited_count; ++index)
        CloseHandle(inherited[index]);
    inherited_count = 0;
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    free(startup.lpAttributeList);
    startup.lpAttributeList = NULL;
    free(image);
    free(command);
    free(environment);
    image = NULL;
    command = NULL;
    environment = NULL;

    /* _open_osfhandle takes ownership of the server handle: close(control)
     * closes it, so `server` must not be closed again on any path below. */
    control = _open_osfhandle((intptr_t)server, 0);
    if (control < 0) goto fail;
    server = NULL;

    if (activation_handshake(control, &request, test_mode, NULL, 0) != 0) {
        status = HL_STATUS_CORRUPT;
        goto fail;
    }
    process = calloc(1, sizeof(*process));
    if (process == NULL) {
        status = HL_STATUS_OUT_OF_MEMORY;
        goto fail;
    }
    process->descriptor = control;
    process->pid = spawned.dwProcessId;
    process->process = spawned.hProcess;
    process->job = job;
    process->domain_job = domain_job;
    memcpy(process->nonce, request.nonce, sizeof(process->nonce));
    *out_process = process;
    return HL_STATUS_OK;

fail:
    if (spawned.hProcess != NULL) {
        (void)TerminateJobObject(job, HL_ACTIVATION_WINDOWS_KILL_CODE);
        (void)WaitForSingleObject(spawned.hProcess, 5000);
        CloseHandle(spawned.hProcess);
    }
    if (startup.lpAttributeList != NULL) {
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        free(startup.lpAttributeList);
    }
    for (index = 0; index < inherited_count; ++index)
        CloseHandle(inherited[index]);
    if (control >= 0) (void)close(control);
    if (server != NULL) CloseHandle(server);
    if (job != NULL) CloseHandle(job);
    if (domain_job != NULL) CloseHandle(domain_job);
    free(image);
    free(command);
    free(environment);
    return status;
}

#endif /* !_WIN32 */

hl_status hl_activation_start_with_stdio(const char *executable, uint32_t guest_isa, const char *guest,
                                         const hl_activation_stdio *stdio, hl_activation_process **out_process) {
    return activation_start(executable, guest_isa, guest, stdio, NULL, NULL, -1, -1, -1, out_process);
}

hl_status hl_activation_start_with_transport(const char *executable, uint32_t guest_isa, const char *guest,
                                             const hl_activation_stdio *stdio, int32_t transport,
                                             hl_activation_process **out_process) {
    if (transport < 0) return HL_STATUS_INVALID_ARGUMENT;
    return activation_start(executable, guest_isa, guest, stdio, NULL, NULL, transport, -1, -1, out_process);
}

hl_status hl_activation_start_terminal(const char *executable, uint32_t guest_isa, const char *guest,
                                       hl_terminal_size size, int32_t *out_master,
                                       hl_activation_process **out_process) {
    return activation_start(executable, guest_isa, guest, NULL, &size, out_master, -1, -1, -1, out_process);
}

hl_status hl_activation_start_terminal_with_transport(const char *executable, uint32_t guest_isa, const char *guest,
                                                      hl_terminal_size size, int32_t transport, int32_t *out_master,
                                                      hl_activation_process **out_process) {
    if (transport < 0) return HL_STATUS_INVALID_ARGUMENT;
    return activation_start(executable, guest_isa, guest, NULL, &size, out_master, transport, -1, -1, out_process);
}

hl_status hl_activation_start_with_channels(const char *executable, uint32_t guest_isa, const char *guest,
                                            const hl_activation_stdio *stdio, const hl_terminal_size *size,
                                            int32_t transport, int32_t checkpoint, int32_t trigger, int32_t *out_master,
                                            hl_activation_process **out_process) {
    return activation_start(executable, guest_isa, guest, stdio, size, out_master, transport, checkpoint, trigger,
                            out_process);
}

hl_status hl_terminal_resize(int32_t master, hl_terminal_size size) {
#if defined(_WIN32)
    /* The counterpart of the terminal refusal in activation_start: there is no
     * master here to resize, because no start on this host produces one. The
     * argument check still runs first so a caller that passes nonsense learns
     * that instead -- an unsupported operation and an invalid argument are
     * different bugs and must not be conflated. */
    if (master < 0 || size.rows == 0 || size.columns == 0) return HL_STATUS_INVALID_ARGUMENT;
    return HL_STATUS_NOT_SUPPORTED;
#else
    struct winsize native = {.ws_row = size.rows, .ws_col = size.columns};
    if (master < 0 || size.rows == 0 || size.columns == 0) return HL_STATUS_INVALID_ARGUMENT;
    return ioctl(master, TIOCSWINSZ, &native) == 0 ? HL_STATUS_OK : HL_STATUS_PLATFORM_FAILURE;
#endif
}

/* HL_ACTIVATION_DESCRIPTOR_NONE is the API's "absent"; -1 is what everything below this boundary has
 * always used for it. A value too wide to be a descriptor number is rejected rather than truncated, so a
 * caller that passed a whole 64-bit handle by mistake gets an error instead of its low half. Returns 0 on
 * success, -1 on a value this API cannot carry. */
static int activation_native_descriptor(hl_activation_descriptor value, int *out) {
    if (value == HL_ACTIVATION_DESCRIPTOR_NONE) {
        *out = -1;
        return 0;
    }
    if (value > (hl_activation_descriptor)INT32_MAX) return -1;
    *out = (int)value;
    return 0;
}

hl_status hl_activation_start_with_streams(const char *executable, uint32_t guest_isa, const char *guest,
                                           const hl_activation_streams *streams, const hl_terminal_size *size,
                                           hl_activation_descriptor transport, hl_activation_descriptor checkpoint,
                                           hl_activation_descriptor trigger, hl_activation_descriptor *out_master,
                                           hl_activation_process **out_process) {
    hl_activation_stdio native_streams = {.input = -1, .output = -1, .error = -1};
    int32_t master = -1;
    int native_transport;
    int native_checkpoint;
    int native_trigger;
    hl_status status;
    if (out_master != NULL) *out_master = HL_ACTIVATION_DESCRIPTOR_NONE;
    if (activation_native_descriptor(transport, &native_transport) != 0 ||
        activation_native_descriptor(checkpoint, &native_checkpoint) != 0 ||
        activation_native_descriptor(trigger, &native_trigger) != 0)
        return HL_STATUS_INVALID_ARGUMENT;
    if (streams != NULL) {
        int input;
        int output;
        int error;
        if (activation_native_descriptor(streams->input, &input) != 0 ||
            activation_native_descriptor(streams->output, &output) != 0 ||
            activation_native_descriptor(streams->error, &error) != 0)
            return HL_STATUS_INVALID_ARGUMENT;
        native_streams.input = (int32_t)input;
        native_streams.output = (int32_t)output;
        native_streams.error = (int32_t)error;
    }
    status = activation_start(executable, guest_isa, guest, streams != NULL ? &native_streams : NULL, size,
                              out_master != NULL ? &master : NULL, native_transport, native_checkpoint, native_trigger,
                              out_process);
    /* activation_start leaves master at -1 whenever no terminal was produced, which is exactly the case
     * this API reports as NONE. */
    if (out_master != NULL && master >= 0) *out_master = (hl_activation_descriptor)master;
    return status;
}

hl_status hl_activation_terminal_resize(hl_activation_descriptor master, hl_terminal_size size) {
    int native;
    if (activation_native_descriptor(master, &native) != 0 || native < 0) return HL_STATUS_INVALID_ARGUMENT;
    return hl_terminal_resize((int32_t)native, size);
}

hl_status hl_activation_start(const char *executable, uint32_t guest_isa, const char *config_path,
                              hl_activation_process **out_process) {
    return hl_activation_start_with_stdio(executable, guest_isa, config_path, NULL, out_process);
}

hl_status hl_activation_process_id(const hl_activation_process *process, uint64_t *out_process_id) {
    if (process == NULL || out_process_id == NULL) return HL_STATUS_INVALID_ARGUMENT;
    *out_process_id = (uint64_t)process->pid;
    return HL_STATUS_OK;
}

#if defined(_WIN32)
/*
 * The Windows arm of "reap the child and decode its status".
 *
 * waitpid's two questions -- did it exit or was it signalled, and with what --
 * are one question here, because a Windows process has only an exit code. The
 * host backend already owns the encoding that keeps the two apart: a forced
 * termination stamps 0xE0484C00 + signal, which is the customer-defined NTSTATUS
 * space and therefore disjoint from every value a main() can return and from
 * every real status. So the decode below is exact rather than heuristic, and it
 * fills the same `waited` word the shared code goes on to read with WIFEXITED
 * and friends -- those macros are host_wait.h's Linux-encoding decoders on this
 * host, which is precisely what makes reusing them correct.
 */
static int wait_child_process(hl_activation_process *process, int *waited) {
    DWORD code = 0;
    if (process->process == NULL) return -1;
    if (WaitForSingleObject((HANDLE)process->process, INFINITE) != WAIT_OBJECT_0) return -1;
    if (!GetExitCodeProcess((HANDLE)process->process, &code)) return -1;
    CloseHandle((HANDLE)process->process);
    process->process = NULL;
    if (code > 0xE0484C00u && code <= 0xE0484C00u + 64u)
        *waited = (int)(code - 0xE0484C00u); /* Linux encoding: termsig in bits 0-6. */
    else
        *waited = (int)((code & 0xFFu) << 8); /* exited, status in bits 8-15. */
    return 0;
}
#else
static int wait_child_process(hl_activation_process *process, int *waited) {
    return wait_child(process->pid, waited);
}
#endif

hl_status hl_activation_wait(hl_activation_process *process, hl_engine_exit *out_exit) {
    hl_activation_reply reply;
    int waited = 0;
    if (process == NULL || out_exit == NULL) return HL_STATUS_INVALID_ARGUMENT;
    if (process->finished) {
        *out_exit = process->final_exit;
        return process->final_status;
    }
    if (transfer(process->descriptor, &reply, sizeof(reply), 0) != 0) {
        (void)close(process->descriptor);
        if (wait_child_process(process, &waited) != 0) {
            cache_failure(process, HL_STATUS_PLATFORM_FAILURE);
            *out_exit = process->final_exit;
            return process->final_status;
        }
        if (WIFSIGNALED(waited)) {
            process->finished = 1;
            process->final_status = HL_STATUS_OK;
            process->final_exit = (hl_engine_exit){.abi = HL_ENGINE_ABI,
                                                   .size = sizeof(process->final_exit),
                                                   .kind = HL_ENGINE_EXIT_SIGNAL,
                                                   .guest_status = WTERMSIG(waited)};
            *out_exit = process->final_exit;
            return HL_STATUS_OK;
        }
        cache_failure(process, HL_STATUS_CORRUPT);
        *out_exit = process->final_exit;
        return process->final_status;
    }
    (void)close(process->descriptor);
    if (wait_child_process(process, &waited) != 0) {
        cache_failure(process, HL_STATUS_PLATFORM_FAILURE);
        *out_exit = process->final_exit;
        return process->final_status;
    }
    if (reply.magic != HL_ACTIVATION_MAGIC || reply.abi != HL_ACTIVATION_ABI || reply.size != sizeof(reply) ||
        memcmp(reply.nonce, process->nonce, sizeof(process->nonce)) != 0 || reply.status < HL_STATUS_OK ||
        reply.status > HL_STATUS_ADDRESS_IN_USE || !WIFEXITED(waited) ||
        (reply.status == HL_STATUS_OK ? WEXITSTATUS(waited) != 0 : WEXITSTATUS(waited) != 127)) {
        cache_failure(process, HL_STATUS_CORRUPT);
        *out_exit = process->final_exit;
        return process->final_status;
    }
    process->finished = 1;
    process->final_status = (hl_status)reply.status;
    process->final_exit = reply.result;
    *out_exit = process->final_exit;
    return process->final_status;
}

hl_status hl_activation_try_wait(hl_activation_process *process, uint32_t *out_ready, hl_engine_exit *out_exit) {
    struct pollfd descriptor;
    int ready;
    if (process == NULL || out_ready == NULL || out_exit == NULL) return HL_STATUS_INVALID_ARGUMENT;
    if (process->finished) {
        *out_ready = 1;
        *out_exit = process->final_exit;
        return process->final_status;
    }
    descriptor = (struct pollfd){.fd = process->descriptor, .events = POLLIN | POLLHUP};
    do {
        ready = poll(&descriptor, 1, 0);
    } while (ready < 0 && errno == EINTR);
    if (ready < 0) return HL_STATUS_PLATFORM_FAILURE;
    if (ready == 0) {
        *out_ready = 0;
        return HL_STATUS_OK;
    }
    *out_ready = 1;
    return hl_activation_wait(process, out_exit);
}

hl_status hl_activation_kill(hl_activation_process *process) {
    if (process == NULL) return HL_STATUS_INVALID_ARGUMENT;
    if (process->finished) return HL_STATUS_BUSY;
#if defined(_WIN32)
    /* kill(-pid, SIGKILL) in the one Windows object that has those semantics.
     * TerminateProcess would reach the engine child alone and orphan every guest
     * process it had spawned; the job holds all of them, including any that
     * changed process group, and terminates the set atomically. */
    if (process->job == NULL) return HL_STATUS_PLATFORM_FAILURE;
    return TerminateJobObject((HANDLE)process->job, HL_ACTIVATION_WINDOWS_KILL_CODE) ? HL_STATUS_OK
                                                                                     : HL_STATUS_PLATFORM_FAILURE;
#else
    return kill(-process->pid, SIGKILL) == 0 ? HL_STATUS_OK : HL_STATUS_PLATFORM_FAILURE;
#endif
}

void hl_activation_process_destroy(hl_activation_process *process) {
    hl_engine_exit ignored;
    if (process == NULL) return;
    if (!process->finished) {
        (void)hl_activation_kill(process);
        (void)hl_activation_wait(process, &ignored);
    }
#if defined(_WIN32)
    /* Neither job carries KILL_ON_JOB_CLOSE, so releasing them here does not
     * terminate anything: a guest that outlived its initial process stays alive
     * exactly as it would on a POSIX host after the parent stopped waiting. The
     * domain job survives as long as any other holder -- another activation in
     * the same domain, or a live member -- keeps it referenced. */
    if (process->process != NULL) CloseHandle((HANDLE)process->process);
    if (process->job != NULL) CloseHandle((HANDLE)process->job);
    if (process->domain_job != NULL) CloseHandle((HANDLE)process->domain_job);
#endif
    free(process);
}

hl_status hl_activation_spawn(const char *executable, uint32_t guest_isa, const char *config_path,
                              hl_engine_exit *out_exit) {
    hl_activation_process *process = NULL;
    hl_status status;
    if (out_exit == NULL) return HL_STATUS_INVALID_ARGUMENT;
    status = hl_activation_start(executable, guest_isa, config_path, &process);
    if (status == HL_STATUS_OK) status = hl_activation_wait(process, out_exit);
    hl_activation_process_destroy(process);
    return status;
}
