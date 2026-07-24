/* Guest fixture for repeated capture/restore cycles.
 *
 * The process boots exactly once, then advances an in-memory counter while it
 * waits for the host to publish a per-stage release file. Every stage line it
 * writes carries the live counter, and the boot line is written only by a
 * genuinely fresh process. A restore that silently relaunched the guest would
 * therefore emit a second BOOT line and reset the counter, both of which the
 * host-side test rejects.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static volatile unsigned long state;

static int advance_until(const char *path) {
    for (;;) {
        state += 7ul;
        if (access(path, F_OK) == 0) return 0;
        if (errno != ENOENT) return 1;
    }
}

int main(int argc, char **argv) {
    char output[1024];
    char release[1024];
    int fd;
    int stage;
    if (argc != 2) return 2;
    if (snprintf(output, sizeof output, "%s.output", argv[1]) >= (int)sizeof output) return 2;
    fd = open(output, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) return 2;
    if (fd > STDERR_FILENO) close(fd);
    fd = open("/dev/null", O_RDONLY);
    if (fd < 0 || dup2(fd, STDIN_FILENO) < 0) return 2;
    if (fd > STDERR_FILENO) close(fd);

    state = 1;
    dprintf(STDOUT_FILENO, "BOOT %ld\n", (long)getpid());
    for (stage = 1; stage <= 3; stage++) {
        dprintf(STDOUT_FILENO, "STAGE %d %lu\n", stage, state);
        if (stage == 3) break;
        if (snprintf(release, sizeof release, "%s.go%d", argv[1], stage) >= (int)sizeof release)
            return 2;
        if (advance_until(release) != 0) return 30 + stage;
    }
    if (state <= 1) return 40;
    dprintf(STDOUT_FILENO, "CYCLE-RESTORED %lu\n", state);
    return 0;
}
