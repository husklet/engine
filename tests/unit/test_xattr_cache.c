#include "../../src/linux_abi/xattr.h"

#include <assert.h>

int main(void) {
    assert(!hl_xattr_cache_is_negative(7, 11));
    hl_xattr_cache_record_negative(7, 11);
    assert(hl_xattr_cache_is_negative(7, 11));
    assert(!hl_xattr_cache_is_negative(7, 12));
    hl_xattr_cache_invalidate();
    assert(!hl_xattr_cache_is_negative(7, 11));
    return 0;
}
