#include "test.h"

#include "../../src/linux_abi/container/namespace.h"

#include <string.h>

int main(void) {
    char root[32] = "/root/one";
    size_t root_length = strlen(root);
    struct hl_linux_vfs_lower lowers[HL_LINUX_VFS_LOWER_CAPACITY] = {0};
    int lower_count = 0;
    struct hl_linux_vfs_namespace view = {root, &root_length, lowers, &lower_count};

    HL_CHECK(strcmp(view.root_canonical, "/root/one") == 0);
    HL_CHECK(hl_linux_vfs_root_length(&view) == 9);
    HL_CHECK(hl_linux_vfs_lower_count(&view) == 0);

    strcpy(lowers[0].canon, "/lower");
    lowers[0].clen = 6;
    lower_count = 1;
    root_length = 5;
    HL_CHECK(hl_linux_vfs_root_length(&view) == 5);
    HL_CHECK(hl_linux_vfs_lower_count(&view) == 1);
    HL_CHECK(strcmp(view.lowers[0].canon, "/lower") == 0);
    HL_CHECK(view.lowers[0].clen == 6);
    return EXIT_SUCCESS;
}
