// Memory-ordering torture: publish a payload via a flag another thread spins on.
// If store/load ordering is lost (x86-TSO barriers wrongly elided after threading), the
// consumer can observe flag==1 while the payload is still torn/stale -> a mismatch.
//
// Producer: fill payload[0..N-1] = seq, then (StoreStore ordering) set flag = seq.
// Consumer: spin until flag==seq, then (LoadLoad ordering) read every payload[i]; all must == seq.
// x86-TSO guarantees the consumer never sees the new flag with a stale payload. We run many rounds
// across many iterations to stress the just-threaded window.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 64
#define ROUNDS 200000

static volatile unsigned payload[N];
static volatile unsigned flag; // plain (not atomic): relies on hardware/TSO ordering the engine must preserve
static volatile int torn = 0;

static void *consumer(void *arg) {
    (void)arg;
    unsigned last = 0;
    for (;;) {
        unsigned f;
        // spin for the next published round
        while ((f = flag) == last) {
            if (f == (unsigned)-1) return NULL; // producer done
        }
        if (f == (unsigned)-1) return NULL;
        // flag advanced to f: under x86-TSO every payload lane must ALREADY be at least f (the payload
        // stores precede the flag store, and the consumer's payload loads follow its flag load). A lane
        // still BEHIND the published flag (< f) is a StoreStore/LoadLoad reorder = the ordering bug. A lane
        // AHEAD (> f) is the free-running producer already publishing a later round -- TSO-legal (real HW
        // and qemu both do it), so it is NOT a violation.
        for (int i = 0; i < N; i++) {
            unsigned v = payload[i];
            if (v < f) {
                torn = 1;
                fprintf(stderr, "TORN(stale) round=%u lane=%d saw=%u\n", f, i, v);
                return NULL;
            }
        }
        last = f;
    }
}

int main(void) {
    pthread_t c;
    if (pthread_create(&c, NULL, consumer, NULL) != 0) return 2;
    for (unsigned r = 1; r <= ROUNDS; r++) {
        for (int i = 0; i < N; i++) payload[i] = r; // publish payload
        flag = r;                                    // then the flag (StoreStore edge)
        if (torn) break;
    }
    flag = (unsigned)-1; // sentinel: stop consumer
    pthread_join(c, NULL);
    if (torn) {
        printf("FAIL: torn/reordered observation\n");
        return 1;
    }
    printf("OK: %d rounds, no torn observation\n", ROUNDS);
    return 0;
}
