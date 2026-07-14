#include "test.h"

#include "../../src/linux_abi/open_plan.h"

#include <string.h>

static int build(const char *path, uint32_t intent, uint32_t overlay, uint32_t read_only, uint32_t symlink,
                 hl_open_plan *plan) {
    hl_open_request request = {path, strlen(path), 41, intent, overlay, read_only, symlink};
    return hl_open_plan_build(&request, plan) == HL_STATUS_OK;
}

int main(void) {
    hl_open_plan plan;
    HL_CHECK(build("/proc/self/stat", HL_OPEN_READ, 0, 0, 0, &plan));
    HL_CHECK(plan.kind == HL_OPEN_SYNTHETIC && plan.synthetic == HL_OPEN_SYNTHETIC_PROC);
    HL_CHECK(build("/sys/devices/system/cpu/online", HL_OPEN_READ, 0, 0, 0, &plan));
    HL_CHECK(plan.kind == HL_OPEN_SYNTHETIC && plan.synthetic == HL_OPEN_SYNTHETIC_SYS);
    HL_CHECK(build("/dev/null", HL_OPEN_WRITE, 0, 0, 0, &plan));
    HL_CHECK(plan.kind == HL_OPEN_SYNTHETIC && plan.synthetic == HL_OPEN_SYNTHETIC_DEV);

    HL_CHECK(build("/usr/lib/libc.so", HL_OPEN_READ, 1, 0, 0, &plan));
    HL_CHECK(plan.kind == HL_OPEN_OVERLAY && plan.overlay == HL_OPEN_OVERLAY_LOOKUP);
    HL_CHECK(build("/etc/config", HL_OPEN_WRITE, 1, 0, 0, &plan));
    HL_CHECK(plan.overlay == HL_OPEN_OVERLAY_COPY_UP);
    HL_CHECK(build("/tmp/new", HL_OPEN_WRITE | HL_OPEN_CREATE, 1, 0, 0, &plan));
    HL_CHECK(plan.overlay == HL_OPEN_OVERLAY_CREATE);

    HL_CHECK(build("/../../etc/./passwd", HL_OPEN_READ, 0, 0, 0, &plan));
    HL_CHECK(plan.kind == HL_OPEN_HOST_PATH && strcmp(plan.path, "/etc/passwd") == 0);
    HL_CHECK(build("link", HL_OPEN_READ | HL_OPEN_PATH_ONLY | HL_OPEN_NOFOLLOW, 0, 0, 1, &plan));
    HL_CHECK(plan.kind == HL_OPEN_HOST_PATH && plan.names_symlink == 1);
    HL_CHECK(build("dir", HL_OPEN_WRITE | HL_OPEN_TEMPORARY, 0, 0, 0, &plan));
    HL_CHECK(plan.kind == HL_OPEN_TMPFILE);
    HL_CHECK(build("/readonly/file", HL_OPEN_WRITE, 0, 1, 0, &plan));
    HL_CHECK(plan.kind == HL_OPEN_ERROR && plan.error == HL_STATUS_PERMISSION_DENIED);
    HL_CHECK(!build("", HL_OPEN_READ, 0, 0, 0, &plan));
    return EXIT_SUCCESS;
}
