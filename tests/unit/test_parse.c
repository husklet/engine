#include "test.h"

#include "../../src/linux_abi/parse.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

typedef void (*invalid_case)(void);

static void invalid_number(void) {
    (void)hl_parse_u64("number", "12x", 0, 20);
}

static void invalid_range(void) {
    (void)hl_parse_port("port", "0");
}

static void invalid_field(void) {
    (void)hl_parse_port_field("field", "", NULL);
}

static int exits_two(invalid_case run) {
    pid_t child = fork();
    HL_CHECK(child >= 0);
    if (child == 0) {
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }
        run();
        _exit(0);
    }
    int status = 0;
    HL_CHECK(waitpid(child, &status, 0) == child);
    return WIFEXITED(status) && WEXITSTATUS(status) == 2;
}

int main(void) {
    HL_CHECK(hl_parse_u64("number", "0", 0, 9) == 0);
    HL_CHECK(hl_parse_u64("number", "9", 0, 9) == 9);
    HL_CHECK(hl_parse_id("uid", "2147483647") == 2147483647);
    HL_CHECK(hl_parse_port("port", "65535") == 65535);
    const char ports[] = "8080:80";
    HL_CHECK(hl_parse_port_field("host", ports, ports + 4) == 8080);
    HL_CHECK(exits_two(invalid_number));
    HL_CHECK(exits_two(invalid_range));
    HL_CHECK(exits_two(invalid_field));
    return EXIT_SUCCESS;
}
