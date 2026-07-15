#include "test.h"

#include "../../src/core/fatal.h"
#include "hl/log.h"

#include <string.h>

typedef struct fatal_capture {
    unsigned calls;
    uint32_t tag;
    char message[64];
    size_t size;
} fatal_capture;

static void fatal_emit(void *opaque, uint32_t tag, const char *message, size_t size) {
    fatal_capture *capture = opaque;
    size_t copy = size < sizeof(capture->message) ? size : sizeof(capture->message);
    capture->calls++;
    capture->tag = tag;
    capture->size = size;
    memcpy(capture->message, message, copy);
}

int main(void) {
    static const hl_host_log_services log = {HL_HOST_LOG_ABI, sizeof(log), fatal_emit};
    fatal_capture capture = {0};
    hl_host_services host = {
        .abi = HL_HOST_SERVICES_ABI,
        .size = sizeof(host),
        .capabilities = HL_HOST_CAP_LOG,
        .context = &capture,
        .log = &log,
    };
    hl_fatal_context fatal;
    static const char message[] = "unable to publish translated code";
#if defined(HL_ENABLE_LOGGING) && HL_ENABLE_LOGGING
    const unsigned expected_calls = 1;
#else
    const unsigned expected_calls = 0;
#endif

    hl_fatal_context_init(&fatal, &host);
    HL_CHECK(hl_fatal_status(&fatal) == HL_STATUS_OK);
    HL_CHECK(hl_fatal_report(&fatal, HL_STATUS_PLATFORM_FAILURE, HL_LOG_TAG_JIT, message, sizeof(message) - 1) ==
             HL_STATUS_PLATFORM_FAILURE);
#if defined(HL_ENABLE_LOGGING) && HL_ENABLE_LOGGING
    HL_CHECK(capture.calls == 1 && capture.tag == HL_LOG_TAG_JIT && capture.size == sizeof(message) - 1);
    HL_CHECK(memcmp(capture.message, message, sizeof(message) - 1) == 0);
#else
    HL_CHECK(capture.calls == 0);
#endif
    HL_CHECK(hl_fatal_report(&fatal, HL_STATUS_OUT_OF_MEMORY, HL_LOG_TAG_TRANSLATE, "later", 5) ==
             HL_STATUS_PLATFORM_FAILURE);
    HL_CHECK(hl_fatal_status(&fatal) == HL_STATUS_PLATFORM_FAILURE);
#if defined(HL_ENABLE_LOGGING) && HL_ENABLE_LOGGING
    HL_CHECK(capture.calls == 1);
#else
    HL_CHECK(capture.calls == 0);
#endif

    host.capabilities = 0;
    hl_fatal_context_init(&fatal, &host);
    HL_CHECK(hl_fatal_report(&fatal, HL_STATUS_RESOURCE_LIMIT, HL_LOG_TAG_JIT, message, sizeof(message) - 1) ==
             HL_STATUS_RESOURCE_LIMIT);
    HL_CHECK(capture.calls == expected_calls);
    host.capabilities = HL_HOST_CAP_LOG;
    host.log = NULL;
    hl_fatal_context_init(&fatal, &host);
    HL_CHECK(hl_fatal_report(&fatal, HL_STATUS_IO, HL_LOG_TAG_JIT, message, sizeof(message) - 1) == HL_STATUS_IO);
    HL_CHECK(capture.calls == expected_calls);
    return EXIT_SUCCESS;
}
