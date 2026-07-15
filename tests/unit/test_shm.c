#include "test.h"
#include "../../src/linux_abi/container/shm.h"
#include <string.h>

int main(void) {
    char path[128];
    HL_CHECK(hl_shm_path("/tmp/x", "/box", "key", path, sizeof(path)) == NULL);
    HL_CHECK(hl_shm_path("/dev/shm/object", "/box", "key", path, sizeof(path)) == path);
    HL_CHECK(strcmp(path, "/box/dev/shm/object") == 0);
    HL_CHECK(hl_shm_path("/dev/shm/a/b", "", "scope", path, sizeof(path)) == path);
    HL_CHECK(strcmp(path, "/tmp/.hl-shm-scope-a_b") == 0);
    HL_CHECK(hl_shm_path("/dev/shm/x", "", "", path, sizeof(path)) == path);
    HL_CHECK(strcmp(path, "/tmp/.hl-shm-unscoped-x") == 0);
    HL_CHECK(hl_shm_path("/dev/shm/toolong", "/box", "key", path, 4) == NULL);
    return 0;
}
