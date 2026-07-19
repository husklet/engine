#include "test.h"

#include "../../src/linux_abi/syscall/provider_epoll_registry.h"

static int retire(ep_provider_watch *watch) {
    if (!ep_provider_retire_begin(watch)) return 0;
    HL_CHECK(atomic_load(&watch->callbacks) == 0);
    ep_provider_retire_finish(watch);
    return 1;
}

int main(void) {
    ep_provider_watch watches[4] = {0};
    ep_provider_watch *first = ep_provider_alloc(watches, 4);
    ep_provider_watch *second;
    HL_CHECK(first == &watches[0]);

    ep_provider_activate(first, 10, 1, 30, 7, 1, 70, 1, 1, 100);
    HL_CHECK(ep_provider_find(watches, 4, 10, 1, 30, 7) == first);
    HL_CHECK(ep_provider_find(watches, 4, 10, 2, 30, 7) == NULL);
    HL_CHECK(ep_provider_find(watches, 4, 10, 1, 30, 8) == NULL);

    /* One provider descriptor may be present in multiple epoll instances. */
    second = ep_provider_alloc(watches, 4);
    HL_CHECK(second == &watches[1]);
    ep_provider_activate(second, 11, 4, 30, 7, 2, 70, 1, 1, 200);
    HL_CHECK(ep_provider_find(watches, 4, 10, 1, 30, 7) == first);
    HL_CHECK(ep_provider_find(watches, 4, 11, 4, 30, 7) == second);

    /* MOD in the first epoll leaves the second epoll's membership and payload
     * untouched, then DEL removes only the second membership. */
    ep_provider_watch *modified = ep_provider_alloc(watches, 4);
    HL_CHECK(modified == &watches[2]);
    ep_provider_activate(modified, 10, 1, 30, 7, 3, 70, 4, 2, 101);
    HL_CHECK(retire(first));
    HL_CHECK(ep_provider_find(watches, 4, 10, 1, 30, 7) == modified && modified->data == 101);
    HL_CHECK(ep_provider_find(watches, 4, 11, 4, 30, 7) == second && second->data == 200);
    HL_CHECK(retire(second));
    HL_CHECK(ep_provider_find(watches, 4, 11, 4, 30, 7) == NULL);
    first = modified;

    /* DEL frees precisely one membership. ADD can reuse its fixed-address slot;
     * stale endpoint generations cannot find the replacement. */
    HL_CHECK(retire(first));
    HL_CHECK(!retire(first));
    HL_CHECK(ep_provider_find(watches, 4, 10, 1, 30, 7) == NULL);
    first = ep_provider_alloc(watches, 4);
    HL_CHECK(first != NULL);
    ep_provider_activate(first, 10, 2, 30, 8, 4, 71, 1, 1, 300);
    HL_CHECK(ep_provider_find(watches, 4, 10, 1, 30, 7) == NULL);
    HL_CHECK(ep_provider_find(watches, 4, 10, 2, 30, 8) == first);

    /* MOD is staged in another fixed slot.  A failed replacement can be
     * discarded without changing the existing membership. */
    ep_provider_watch *replacement = ep_provider_alloc(watches, 4);
    HL_CHECK(replacement != first);
    ep_provider_activate(replacement, 10, 2, 30, 8, 5, 71, 4, 2, 400);
    HL_CHECK(retire(replacement)); /* provider subscribe failed */
    HL_CHECK(ep_provider_find(watches, 4, 10, 2, 30, 8) == first && first->data == 300);

    /* Readiness transitions: LT retains a sampled-ready condition, ET consumes
     * an edge once, and ONESHOT disarms until MOD installs new interests. */
    int unsubscribe = 0;
    atomic_store(&first->ready, 1);
    HL_CHECK(ep_provider_take_ready(first, 1, &unsubscribe) == 1 && !unsubscribe &&
             atomic_load(&first->ready) == 1);
    first->events = UINT32_C(0x80000001);
    atomic_store(&first->ready, 1);
    HL_CHECK(ep_provider_take_ready(first, 1, &unsubscribe) == 1 && !unsubscribe &&
             atomic_load(&first->ready) == 0);
    first->events = UINT32_C(0x40000001);
    first->interests = 1;
    atomic_store(&first->ready, 1);
    HL_CHECK(ep_provider_take_ready(first, 1, &unsubscribe) == 1 && unsubscribe && first->interests == 0);
    first->events = 1; /* EPOLL_CTL_MOD rearm */
    first->interests = 1;
    atomic_store(&first->ready, 1);
    HL_CHECK(ep_provider_take_ready(first, 0, &unsubscribe) == 1 && !unsubscribe);

    /* Capacity is hard-bounded and generation counters never publish zero. */
    for (uint32_t index = 0; index < 4; ++index)
        if (atomic_load(&watches[index].state) == EP_PROVIDER_FREE) {
            ep_provider_watch *reserved = ep_provider_alloc(watches, 4);
            HL_CHECK(reserved != NULL);
            ep_provider_activate(reserved, 20 + (int)index, 1, 40 + (int)index, 1,
                                 10 + index, 90 + index, 1, 1, index);
        }
    HL_CHECK(ep_provider_alloc(watches, 4) == NULL);
    HL_CHECK(ep_provider_next(UINT32_MAX) == 1 && ep_provider_next(41) == 42);
    HL_CHECK(ep_provider_linux_events(1 | 2 | 4 | 8 | 16) == (0x001u | 0x004u | 0x002u | 0x008u | 0x010u));
    return 0;
}
