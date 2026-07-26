/*
 * jetalloc — hard cross-thread correctness test.
 * SPDX-License-Identifier: MIT
 *
 * Producers allocate blocks and hand them to consumers through per-pair SPSC
 * rings; consumers FREE blocks that a DIFFERENT thread allocated — exercising
 * the batched remote-free message-passing path end to end. Each block carries
 * head/tail canaries verified at free time, so any freelist corruption, block
 * aliasing, or torn cross-thread hand-off is caught.
 */
#include "jetalloc.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define PAIRS   4
#define QCAP    2048
#define XFERS   1500000        /* per pair */

typedef struct { _Atomic(void*) slot[QCAP]; } spsc;
static spsc q[PAIRS];
static int  fails = 0;

/* Encode a size in the block so the consumer can re-derive the tail canary. */
static void* producer(void* arg) {
    long id = (long)arg;
    unsigned seed = (unsigned)id * 2654435761u + 1;
    long head = 0;
    for (long i = 0; i < XFERS; ++i) {
        seed = seed * 1103515245u + 12345u;
        size_t n = 24 + ((seed >> 6) % 200);
        unsigned char* p = (unsigned char*)jet_malloc(n);
        if (!p) { __atomic_fetch_add(&fails, 1, __ATOMIC_RELAXED); continue; }
        /* store size + canaries */
        *(uint32_t*)p = (uint32_t)n;
        p[8] = 0xAB;
        p[n - 1] = 0xCD;
        unsigned s = head & (QCAP - 1);
        while (atomic_load_explicit(&q[id].slot[s], memory_order_acquire) != NULL)
            ;
        atomic_store_explicit(&q[id].slot[s], p, memory_order_release);
        head++;
    }
    return NULL;
}

static void* consumer(void* arg) {
    long id = (long)arg;
    long tail = 0;
    for (long i = 0; i < XFERS; ++i) {
        unsigned s = tail & (QCAP - 1);
        unsigned char* p;
        while ((p = (unsigned char*)atomic_load_explicit(&q[id].slot[s],
                                                         memory_order_acquire)) == NULL)
            ;
        atomic_store_explicit(&q[id].slot[s], NULL, memory_order_release);
        uint32_t n = *(uint32_t*)p;
        if (n < 24 || n > 224 || p[8] != 0xAB || p[n - 1] != 0xCD)
            __atomic_fetch_add(&fails, 1, __ATOMIC_RELAXED);
        jet_free(p);        /* cross-thread free */
        tail++;
    }
    return NULL;
}

int main(void) {
    printf("jetalloc %s — cross-thread (producer/consumer) correctness\n",
           jet_version());
    for (int i = 0; i < PAIRS; ++i)
        for (int j = 0; j < QCAP; ++j)
            atomic_store(&q[i].slot[j], NULL);

    pthread_t p[PAIRS], c[PAIRS];
    for (long i = 0; i < PAIRS; ++i) pthread_create(&c[i], NULL, consumer, (void*)i);
    for (long i = 0; i < PAIRS; ++i) pthread_create(&p[i], NULL, producer, (void*)i);
    for (int i = 0; i < PAIRS; ++i) pthread_join(p[i], NULL);
    for (int i = 0; i < PAIRS; ++i) pthread_join(c[i], NULL);

    if (fails) { printf("%d FAILURES\n", fails); return 1; }
    printf("ALL PASSED (%d cross-thread frees)\n", PAIRS * XFERS);
    return 0;
}
