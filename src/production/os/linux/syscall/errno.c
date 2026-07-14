#include "errno.h"

int hl_linux_errno_from_macos(int host_errno) {
    /* Indexed by Darwin errno; values are Linux errno numbers. Unknown values pass through. */
    static const short linux_errno[107] = {
        0,  1,   2,  3,   4,   5,  6,   7,   8,   9,   10,  35,  12,  13,  14,  15,  16,  17,  18, 19, 20,  21,
        22, 23,  24, 25,  26,  27, 28,  29,  30,  31,  32,  33,  34,  11,  115, 114, 88,  89,  90, 91, 92,  93,
        94, 95,  96, 97,  98,  99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 40, 36, 112, 113,
        39, 22,  87, 122, 116, 66, 22,  22,  22,  22,  22,  37,  38,  22,  22,  22,  22,  22,  75, 22, 22,  22,
        22, 125, 43, 42,  84,  61, 74,  72,  61,  67,  63,  60,  71,  62,  95,  22,  131, 130, 22,
    };
    return host_errno >= 0 && host_errno < (int)(sizeof(linux_errno) / sizeof(linux_errno[0])) ? linux_errno[host_errno]
                                                                                               : host_errno;
}
