#include "test.h"

#include "hl/log.h"

#include <string.h>

typedef struct log_capture {
    uint32_t events;
    uint32_t last_tag;
    char message[64];
} log_capture;

static void capture_emit(void *context, uint32_t event, const char *message, size_t message_size) {
    log_capture *capture = context;
    size_t copy = message_size < sizeof(capture->message) - 1 ? message_size : sizeof(capture->message) - 1;
    capture->events++;
    capture->last_tag = event;
    memcpy(capture->message, message, copy);
    capture->message[copy] = '\0';
}

int main(void) {
    static const hl_host_log_services log_services = {HL_HOST_LOG_ABI, sizeof(log_services), capture_emit};
    log_capture capture = {0};
    hl_host_services host = {0};
    hl_log_context log;
    host.abi = HL_HOST_SERVICES_ABI;
    host.size = sizeof(host);
    host.capabilities = HL_HOST_CAP_LOG;
    host.context = &capture;
    host.log = &log_services;
    HL_CHECK(hl_log_context_init(&log, &host, "log:fs,log:jit") == HL_STATUS_OK);
#if defined(HL_ENABLE_LOGGING) && HL_ENABLE_LOGGING
    HL_CHECK(hl_log_enabled(&log, HL_LOG_TAG_FS));
    HL_CHECK(!hl_log_enabled(&log, HL_LOG_TAG_NETWORK));
    HL_LOG(&log, HL_LOG_TAG_FS, "open");
    HL_LOGF(&log, HL_LOG_TAG_JIT, "block=%d", 7);
    HL_CHECK(capture.events == 2 && capture.last_tag == HL_LOG_TAG_JIT);
    HL_CHECK(strcmp(capture.message, "[hl:jit] block=7\n") == 0);
#else
    int side_effect = 0;
    HL_LOGF(&log, HL_LOG_TAG_FS, "side=%d", ++side_effect);
    HL_CHECK(side_effect == 0 && capture.events == 0);
#endif
    return EXIT_SUCCESS;
}
