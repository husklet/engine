#include "../../src/core/provider_handles.h"
#include "test.h"

#include <errno.h>

int main(void) {
    hl_provider_handles handles = {0};
    hl_host_handle first, reused;
    uint64_t remote = 0;
    HL_CHECK(hl_provider_handle_open(&handles, 41, &first) == 0 && hl_provider_handle_is(first));
    HL_CHECK(hl_provider_handle_get(&handles, first, &remote) == 0 && remote == 41);
    HL_CHECK(hl_provider_handle_retain(&handles, first) == 0);
    HL_CHECK(hl_provider_handle_release(&handles, first, &remote) == 0);
    HL_CHECK(hl_provider_handle_get(&handles, first, &remote) == 0 && remote == 41);
    HL_CHECK(hl_provider_handle_release(&handles, first, &remote) == 1 && remote == 41);
    HL_CHECK(hl_provider_handle_get(&handles, first, &remote) == -EBADF);
    HL_CHECK(hl_provider_handle_open(&handles, 42, &reused) == 0 && reused != first);
    HL_CHECK(hl_provider_handle_get(&handles, first, &remote) == -EBADF);
    HL_CHECK(hl_provider_handle_get(&handles, reused, &remote) == 0 && remote == 42);
    hl_provider_handles_revoke(&handles);
    HL_CHECK(hl_provider_handle_get(&handles, reused, &remote) == -EBADF);
    return 0;
}
