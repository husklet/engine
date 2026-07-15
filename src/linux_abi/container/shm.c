#include "shm.h"

#include <stdio.h>
#include <string.h>

const char *hl_shm_path(const char *guest, const char *root, const char *namespace_key, char *output, size_t capacity) {
    if (guest == NULL || output == NULL || capacity == 0 || guest[0] != '/' || strncmp(guest, "/dev/shm/", 9))
        return NULL;
    const char *name = guest + 9;
    int prefix = root != NULL && root[0]
                     ? snprintf(output, capacity, "%s/dev/shm/", root)
                     : snprintf(output, capacity, "/tmp/.hl-shm-%s-",
                                namespace_key != NULL && namespace_key[0] ? namespace_key : "unscoped");
    if (prefix < 0 || (size_t)prefix >= capacity - 1) return NULL;
    int length = prefix + snprintf(output + prefix, capacity - (size_t)prefix, "%s", name);
    if (length > (int)capacity - 1) length = (int)capacity - 1;
    for (int index = prefix; index < length; ++index)
        if (output[index] == '/') output[index] = '_';
    return output;
}
