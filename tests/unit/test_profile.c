#include "../../src/core/profile.h"

#include <assert.h>

typedef struct report_capture {
    unsigned calls;
    uint64_t translations;
    uint64_t translation_ns;
} report_capture;

static void capture_report(void *context, uint64_t translations, uint64_t translation_ns) {
    report_capture *capture = context;
    capture->calls++;
    capture->translations = translations;
    capture->translation_ns = translation_ns;
}

int main(void) {
    hl_dispatch_profile profile = {0};
    report_capture capture = {0};

    assert(hl_dispatch_profile_begin(&profile, 10) == 0);
    for (uint64_t block = 0; block < 100000; block++)
        hl_dispatch_profile_translation(&profile);
    hl_dispatch_profile_translation_end(&profile, 0, 20);
    assert(capture.calls == 0);
    assert(profile.translations == 100000);
    assert(profile.translation_ns == 0);

    profile.enabled = 1;
    assert(hl_dispatch_profile_begin(&profile, 30) == 30);
    hl_dispatch_profile_crossing(&profile);
    hl_dispatch_profile_translation(&profile);
    hl_dispatch_profile_translation_end(&profile, 30, 47);
    assert(profile.crossings == 1);
    assert(profile.translations == 100001);
    assert(profile.translation_ns == 17);
    assert(capture.calls == 0);

    hl_dispatch_profile_report(&profile, &capture, capture_report);
    assert(capture.calls == 1);
    assert(capture.translations == 100001);
    assert(capture.translation_ns == 17);
    return 0;
}
