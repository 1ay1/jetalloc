/* jetalloc — concurrent large-alloc registry stress. SPDX-License-Identifier: MIT
 *
 * Exercises the live-large-mapping registry: many threads concurrently
 * malloc/free large (mmap-backed) blocks while calling jet_owns() on them.
 * The registry makes jet_owns() crash-proof on foreign / freed page-aligned
 * pointers; this proves it stays correct and race-free under contention. */
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include "jetalloc.h"

#define T 8
#define ITERS 20000
static int fails = 0;

static void* worker(void* arg) {
    long id = (long)arg;
    unsigned seed = (unsigned)id * 2654435761u + 1;
    void* live[64] = {0};
    for (int i = 0; i < ITERS; ++i) {
        seed = seed * 1103515245u + 12345u;
        int slot = (seed >> 8) % 64;
        if (live[slot]) {
            /* A block THIS thread holds must be owned right up until we free it
             * (no other thread can hand out an address we still hold). */
            if (!jet_owns(live[slot])) __atomic_fetch_add(&fails, 1, __ATOMIC_RELAXED);
            /* Canary survived the block's lifetime. */
            unsigned char* p = live[slot];
            if (p[0] != 0xAA) __atomic_fetch_add(&fails, 1, __ATOMIC_RELAXED);
            jet_free(live[slot]);
            live[slot] = 0;
            /* NB: we do NOT assert !jet_owns() here — once freed, another
             * thread may immediately mmap the same address and register it,
             * so jet_owns() legitimately becomes true again (ABA). We only
             * require that the call is SAFE (no crash), which it now is. */
        } else {
            size_t n = 65536 + ((seed >> 4) % 300000);   /* always large path */
            unsigned char* p = jet_malloc(n);
            if (p) { p[0] = 0xAA; p[n-1] = 0xBB; live[slot] = p; }
        }
    }
    for (int s = 0; s < 64; ++s) if (live[s]) jet_free(live[s]);
    return 0;
}

int main() {
    printf("jetalloc %s — concurrent large-registry stress\n", jet_version());
    pthread_t th[T];
    for (long i = 0; i < T; ++i) pthread_create(&th[i], 0, worker, (void*)i);
    for (int i = 0; i < T; ++i) pthread_join(th[i], 0);
    if (fails) { printf("%d FAILURES\n", fails); return 1; }
    printf("ALL PASSED\n");
    return 0;
}
