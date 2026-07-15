#include "test.h"

#include "../../src/translator/guest/x86_64/glue.h"

static hl_host_result monotonic(void *context) {
    (void)context;
    return (hl_host_result){HL_STATUS_OK, 0, UINT64_C(123456789), 0};
}

int main(void) {
    hl_host_clock_services clock = {.abi = HL_HOST_CLOCK_ABI,
                                    .size = sizeof clock,
                                    .monotonic_ns = monotonic};
    hl_host_services services = {.abi = HL_HOST_SERVICES_ABI,
                                 .size = sizeof services,
                                 .clock = &clock};

    g_repmovs_n = 0;
    g_repstos_n = 0;
    hl_x86_count_rep_movs();
    hl_x86_count_rep_stos();
    HL_CHECK(g_repmovs_n == 1 && g_repstos_n == 1);
    HL_CHECK(!ibtc1way() && !nosseopt() && !noeaopt());
    HL_CHECK(g_reloc == g_reloc_storage);
    HL_CHECK(g_nreloc == 0 && g_reloc_table.capacity == (int)PC_RELOC_CAP);
    HL_CHECK(g_notier2x == -1 && notier2x() == -1);
    HL_CHECK(coldprof_now_ns(NULL) == 0);
    HL_CHECK(coldprof_now_ns(&services) == UINT64_C(123456789));
    return EXIT_SUCCESS;
}
