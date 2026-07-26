/*
 * jetalloc — benchmark harness. SPDX-License-Identifier: MIT
 *
 * Compile once against jetalloc (jet_bench) and once against the system
 * allocator (jet_bench_system, -DJET_BENCH_SYSTEM). Same workload, so the
 * wall-clock delta is the allocator delta. Reports Mops/s per workload.
 *
 * Workloads:
 *   1. small-fixed  : tight malloc(32)/free loop (freelist throughput)
 *   2. mixed-size   : random sizes 8..4096, LIFO free (realistic churn)
 *   3. producer/consumer across threads (cross-thread free path)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
#include <pthread.h>

#if defined(JET_BENCH_SYSTEM)
#  define ALLOC(n)   malloc(n)
#  define FREE(p)    free(p)
#  define NAME       "system"
#else
#  include "jetalloc.h"
#  define ALLOC(n)   jet_malloc(n)
#  define FREE(p)    jet_free(p)
#  define NAME       "jetalloc"
#endif

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void bench_small_fixed(void) {
    const long N = 50 * 1000 * 1000;
    double t0 = now_s();
    void* p = NULL;
    for (long i = 0; i < N; ++i) {
        p = ALLOC(32);
        *(volatile char*)p = (char)i;
        FREE(p);
    }
    double dt = now_s() - t0;
    printf("  %-14s small-fixed(32B)   %8.1f Mops/s  (%.2fs)\n",
           NAME, N / dt / 1e6, dt);
}

static void bench_mixed_size(void) {
    const long N = 10 * 1000 * 1000;
    const int  W = 1024;               /* live working set */
    void** live = calloc(W, sizeof(void*));
    unsigned seed = 12345;
    double t0 = now_s();
    for (long i = 0; i < N; ++i) {
        seed = seed * 1103515245u + 12345u;
        int slot = (seed >> 8) & (W - 1);
        if (live[slot]) FREE(live[slot]);
        size_t sz = 8 + ((seed >> 5) % 4088);
        live[slot] = ALLOC(sz);
        *(volatile char*)live[slot] = (char)i;
    }
    for (int i = 0; i < W; ++i) if (live[i]) FREE(live[i]);
    free(live);
    double dt = now_s() - t0;
    printf("  %-14s mixed-size(8-4K)   %8.1f Mops/s  (%.2fs)\n",
           NAME, N / dt / 1e6, dt);
}

#define PC_THREADS 8
/* 40M ops/thread: the old 5M finished in ~0.03 s, which measured thread
 * creation + first-touch page faults far more than allocator steady state
 * (run-to-run spread was as wide as the gaps between allocators). At 40M the
 * run is ~0.3 s and dominated by the actual alloc/free loop. */
#define PC_OPS     (40 * 1000 * 1000)
static void* pc_worker(void* arg) {
    unsigned seed = (unsigned)(uintptr_t)arg * 2654435761u + 1;
    void* keep[64] = {0};
    for (long i = 0; i < PC_OPS; ++i) {
        seed = seed * 1103515245u + 12345u;
        int slot = (seed >> 8) & 63;
        if (keep[slot]) FREE(keep[slot]);
        keep[slot] = ALLOC(8 + ((seed >> 4) % 512));
        *(volatile char*)keep[slot] = (char)i;
    }
    for (int i = 0; i < 64; ++i) if (keep[i]) FREE(keep[i]);
    return NULL;
}
static void bench_threaded(void) {
    pthread_t th[PC_THREADS];
    double t0 = now_s();
    for (long i = 0; i < PC_THREADS; ++i)
        pthread_create(&th[i], NULL, pc_worker, (void*)i);
    for (int i = 0; i < PC_THREADS; ++i) pthread_join(th[i], NULL);
    double dt = now_s() - t0;
    long total = (long)PC_THREADS * PC_OPS;
    printf("  %-14s threaded(%dT)       %8.1f Mops/s  (%.2fs)\n",
           NAME, PC_THREADS, total / dt / 1e6, dt);
}

/* ── Producer/consumer: THE cross-thread workload ─────────────────────────
 * Each producer is paired 1:1 with a consumer through a private SPSC ring, so
 * the benchmark measures the ALLOCATOR's cross-thread path (producer allocs,
 * consumer frees the other thread's memory) rather than shared-queue CAS
 * contention. This is exactly snmalloc's producer/consumer target workload. */
#define PC_PAIRS   4
#define PC_QCAP    4096
#define PC_XFERS   (12 * 1000 * 1000)   /* per pair */
typedef struct { _Atomic(void*) slot[PC_QCAP]; } spsc;
static spsc pcq[PC_PAIRS];

static void* producer(void* arg) {
    long id = (long)arg;
    spsc* q = &pcq[id];
    unsigned seed = (unsigned)id * 0x9E3779B1u + 1;
    long head = 0;
    for (long i = 0; i < PC_XFERS; ++i) {
        seed = seed * 1103515245u + 12345u;
        void* p = ALLOC(16 + ((seed >> 6) % 240));
        *(volatile char*)p = 1;
        unsigned s = head & (PC_QCAP - 1);
        while (atomic_load_explicit(&q->slot[s], memory_order_acquire) != NULL)
            ;                       /* wait for the consumer to drain a slot */
        atomic_store_explicit(&q->slot[s], p, memory_order_release);
        head++;
    }
    return NULL;
}
static void* consumer(void* arg) {
    long id = (long)arg;
    spsc* q = &pcq[id];
    long tail = 0;
    for (long i = 0; i < PC_XFERS; ++i) {
        unsigned s = tail & (PC_QCAP - 1);
        void* p;
        while ((p = atomic_load_explicit(&q->slot[s], memory_order_acquire)) == NULL)
            ;
        atomic_store_explicit(&q->slot[s], NULL, memory_order_release);
        FREE(p);                    /* cross-thread free: producer allocated it */
        tail++;
    }
    return NULL;
}
static void bench_producer_consumer(void) {
    for (int i = 0; i < PC_PAIRS; ++i)
        for (int j = 0; j < PC_QCAP; ++j)
            atomic_store(&pcq[i].slot[j], NULL);
    pthread_t p[PC_PAIRS], c[PC_PAIRS];
    double t0 = now_s();
    for (long i = 0; i < PC_PAIRS; ++i) pthread_create(&c[i], NULL, consumer, (void*)i);
    for (long i = 0; i < PC_PAIRS; ++i) pthread_create(&p[i], NULL, producer, (void*)i);
    for (int i = 0; i < PC_PAIRS; ++i) pthread_join(p[i], NULL);
    for (int i = 0; i < PC_PAIRS; ++i) pthread_join(c[i], NULL);
    double dt = now_s() - t0;
    double total = (double)PC_PAIRS * PC_XFERS;   /* each xfer = 1 alloc+1 free */
    printf("  %-14s prod/cons(%dpr)    %8.1f Mops/s  (%.2fs)\n",
           NAME, PC_PAIRS, total / dt / 1e6, dt);
}

int main(void) {
    printf("== %s ==\n", NAME);
    bench_small_fixed();
    bench_mixed_size();
    bench_threaded();
    bench_producer_consumer();
    return 0;
}
