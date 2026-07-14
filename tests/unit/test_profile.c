#include "../../src/core/profile.h"

#include <assert.h>

int main(void) {
    hl_dispatch_profile profile = {0};

    assert(hl_dispatch_profile_begin(&profile, 10) == 0);
    hl_dispatch_profile_crossing(&profile);
    hl_dispatch_profile_translation(&profile);
    hl_dispatch_profile_translation_end(&profile, 0, 20);
    assert(profile.crossings == 0);
    assert(profile.translations == 1);
    assert(profile.translation_ns == 0);

    profile.enabled = 1;
    assert(hl_dispatch_profile_begin(&profile, 30) == 30);
    hl_dispatch_profile_crossing(&profile);
    hl_dispatch_profile_translation(&profile);
    hl_dispatch_profile_translation_end(&profile, 30, 47);
    assert(profile.crossings == 1);
    assert(profile.translations == 2);
    assert(profile.translation_ns == 17);
    return 0;
}
