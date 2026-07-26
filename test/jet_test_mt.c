/* jetalloc — multithread stress test (cross-thread frees). SPDX: MIT */
#include "jetalloc.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NTHREADS 8
#define NOPS     200000

/* A shared ring of slots: producers alloc into a slot, consumers (other
 * threads) free it — exercising the lock-free cross-thread free path. */
#define SLOTS 4096
static _Atomic(void*) ring[SLOTS];
static int fails = 0;

static void* worker(void* arg) {
    unsigned seed = (unsigned)(uintptr_t)arg * 2654435761u + 1;
    for (int op = 0; op < NOPS; ++op) {
        seed = seed * 1103515245u + 12345u;
        int slot = (seed >> 8) % SLOTS;
        size_t sz = 8 + ((seed >> 3) % 2000);

        void* mine = jet_malloc(sz);
        if (!mine) { __atomic_fetch_add(&fails, 1, __ATOMIC_RELAXED); continue; }
        memset(mine, (int)(op & 0xFF), sz);

        /* Swap our block into the shared ring; free whatever was there
         * (allocated by some OTHER thread → cross-thread free). */
        void* prev = atomic_exchange(&ring[slot], mine);
        if (prev) jet_free(prev);
    }
    return NULL;
}

int main(void) {
    printf("jetalloc %s — %d-thread cross-thread stress (%d ops each)\n",
           jet_version(), NTHREADS, NOPS);
    for (int i = 0; i < SLOTS; ++i) atomic_store(&ring[i], NULL);

    pthread_t th[NTHREADS];
    for (long i = 0; i < NTHREADS; ++i)
        pthread_create(&th[i], NULL, worker, (void*)i);
    for (int i = 0; i < NTHREADS; ++i)
        pthread_join(th[i], NULL);

    /* Drain the ring. */
    for (int i = 0; i < SLOTS; ++i) {
        void* p = atomic_load(&ring[i]);
        if (p) jet_free(p);
    }

    jet_stats s;
    jet_get_stats(&s);
    printf("done. alloc=%zu free=%zu live=%zu mapped=%zu\n",
           s.alloc_calls, s.free_calls, s.bytes_live, s.bytes_mapped);

    if (fails) { printf("%d allocation failures\n", fails); return 1; }
    printf("ALL PASSED\n");
    return 0;
}
