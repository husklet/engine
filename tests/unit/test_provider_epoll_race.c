#include "test.h"

#include "../../src/linux_abi/syscall/provider_epoll_registry.h"

#include <pthread.h>
#include <sched.h>

enum { WATCHES = 32, WORKERS = 8, LOOPS = 10000 };

static ep_provider_watch watches[WATCHES];
static _Atomic uint32_t owners[WATCHES];
static _Atomic uint32_t serials;
static _Atomic uint32_t stop_callbacks;
static _Atomic uint64_t callback_entries;
static _Atomic uint32_t failed;

static uint32_t index_of(ep_provider_watch *watch) {
    return (uint32_t)(watch - watches);
}

static void release(ep_provider_watch *watch, uint32_t owner) {
    uint32_t index = index_of(watch);
    if (!ep_provider_retire_begin(watch)) {
        atomic_store(&failed, 1);
        return;
    }
    while (atomic_load_explicit(&watch->callbacks, memory_order_acquire) != 0) sched_yield();
    uint32_t expected = owner;
    if (!atomic_compare_exchange_strong(&owners[index], &expected, 0)) atomic_store(&failed, 1);
    ep_provider_retire_finish(watch);
}

static void *callbacks(void *opaque) {
    (void)opaque;
    while (!atomic_load(&stop_callbacks)) {
        for (uint32_t index = 0; index < WATCHES; ++index) {
            ep_provider_watch *watch = &watches[index];
            uint64_t token = atomic_load(&watch->serial);
            if (!ep_provider_callback_enter(watch, token)) continue;
            uint32_t serial = atomic_load(&watch->serial);
            sched_yield();
            if (atomic_load(&watch->serial) != serial) atomic_store(&failed, 1);
            atomic_fetch_or(&watch->ready, 1);
            atomic_fetch_add(&callback_entries, 1);
            ep_provider_callback_leave(watch);
        }
    }
    return NULL;
}

static void *mutate(void *opaque) {
    uint32_t owner = (uint32_t)(uintptr_t)opaque;
    for (uint32_t iteration = 0; iteration < LOOPS && !atomic_load(&failed); ++iteration) {
        ep_provider_watch *watch = ep_provider_alloc(watches, WATCHES);
        if (watch == NULL) {
            sched_yield();
            continue;
        }
        uint32_t index = index_of(watch), expected = 0;
        if (!atomic_compare_exchange_strong(&owners[index], &expected, owner)) {
            atomic_store(&failed, 1);
            ep_provider_reservation_cancel(watch);
            break;
        }
        uint32_t serial = atomic_fetch_add(&serials, 1) + 1;
        ep_provider_activate(watch, (int)(owner + 10), iteration + 1, (int)(index + 50), iteration + 1,
                             serial, 100 + index, iteration & 1 ? 1 : UINT32_C(0x80000001), 1, serial);

        /* Stale callback epochs must never enter a newly published record. */
        if (serial > 1 && ep_provider_callback_enter(watch, serial - 1)) {
            atomic_store(&failed, 1);
            ep_provider_callback_leave(watch);
        }

        if ((iteration & 3) == 0) {
            /* Transactional MOD: publish a replacement before retiring the
             * old watch, so callbacks may overlap on distinct stable slots. */
            ep_provider_watch *replacement = ep_provider_alloc(watches, WATCHES);
            if (replacement != NULL) {
                uint32_t replacement_index = index_of(replacement), replacement_expected = 0;
                if (!atomic_compare_exchange_strong(&owners[replacement_index], &replacement_expected, owner)) {
                    atomic_store(&failed, 1);
                    ep_provider_reservation_cancel(replacement);
                } else {
                    uint32_t replacement_serial = atomic_fetch_add(&serials, 1) + 1;
                    ep_provider_activate(replacement, (int)(owner + 10), iteration + 1,
                                         (int)(index + 50), iteration + 1, replacement_serial,
                                         100 + index, 4, 2, replacement_serial);
                    release(watch, owner);
                    watch = replacement;
                }
            }
        }
        release(watch, owner); /* DEL/close */
    }
    return NULL;
}

int main(void) {
    pthread_t workers[WORKERS], pumps[2];
    HL_CHECK(pthread_create(&pumps[0], NULL, callbacks, NULL) == 0);
    HL_CHECK(pthread_create(&pumps[1], NULL, callbacks, NULL) == 0);
    for (uint32_t index = 0; index < WORKERS; ++index)
        HL_CHECK(pthread_create(&workers[index], NULL, mutate, (void *)(uintptr_t)(index + 1)) == 0);
    for (uint32_t index = 0; index < WORKERS; ++index) HL_CHECK(pthread_join(workers[index], NULL) == 0);
    atomic_store(&stop_callbacks, 1);
    HL_CHECK(pthread_join(pumps[0], NULL) == 0 && pthread_join(pumps[1], NULL) == 0);
    HL_CHECK(!atomic_load(&failed));
    HL_CHECK(atomic_load(&callback_entries) != 0);
    HL_CHECK(atomic_load(&serials) > LOOPS);
    for (uint32_t index = 0; index < WATCHES; ++index) {
        HL_CHECK(atomic_load(&watches[index].state) == EP_PROVIDER_FREE);
        HL_CHECK(atomic_load(&watches[index].callbacks) == 0);
        HL_CHECK(atomic_load(&owners[index]) == 0);
    }
    return 0;
}
