#include "../../src/core/provider_files.h"
#include "test.h"

#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>

static int marker;
static int mismatch;
static hl_host_result ordinary_read(void *context, hl_host_handle file, uint64_t offset, hl_host_bytes output) {
    if (context != &marker || file != 123 || offset != 7 || output.size != 1) mismatch = 1;
    *(unsigned char *)output.data = 0xa5;
    return (hl_host_result){.status = HL_STATUS_OK, .value = 1, .detail = 77};
}

int main(void) {
    int pair[2];
    hl_provider_client client;
    hl_host_file_services files = {.abi = HL_HOST_FILE_ABI, .size = sizeof(files), .read_at = ordinary_read};
    hl_host_services services = {.abi = HL_HOST_SERVICES_ABI, .size = sizeof(services), .context = &marker,
                                 .file = &files};
    unsigned char byte = 0;
    hl_host_result result;
    HL_CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    HL_CHECK(hl_provider_client_init(&client, pair[0], 1024) == 0);
    HL_CHECK(hl_provider_files_install(&services, &client) == 0 && services.file != &files);
    result = services.file->read_at(services.context, 123, 7, (hl_host_bytes){.data = &byte, .size = 1});
    HL_CHECK(result.status == HL_STATUS_OK && result.value == 1 && result.detail == 77 && byte == 0xa5 && !mismatch);
    hl_provider_files_revoke();
    hl_provider_client_destroy(&client);
    close(pair[0]); close(pair[1]);
    return 0;
}
