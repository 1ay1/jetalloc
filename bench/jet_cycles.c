/*
 * jetalloc — cycle-precise microbenchmark for the hot paths.
 * Measures median cycles/op via rdtsc over the drop-in malloc/free interface.
 * Build: gcc -O3 -o jet_cycles bench/jet_cycles.c -lpthread
 * Run:   LD_PRELOAD=./build/libjetalloc.so ./jet_cycles
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <x86intrin.h>

#ifndef NAME
#define NAME "system"
#endif

static int cmp_u64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Median-of-N cycles for a tight malloc/free churn at one size. */
static double churn_cycles(size_t sz, int iters, int reps) {
    uint64_t samples[64];
    if (reps > 64) reps = 64;
    void** slots = calloc(1024, sizeof(void*));
    /* warm up */
    for (int i = 0; i < 1024; ++i) { slots[i] = malloc(sz); }
    for (int i = 0; i < 1024; ++i) { free(slots[i]); slots[i] = NULL; }
    for (int r = 0; r < reps; ++r) {
        _mm_lfence();
        uint64_t t0 = __rdtsc();
        _mm_lfence();
        for (int i = 0; i < iters; ++i) {
            unsigned k = (unsigned)(i & 1023);
            if (slots[k]) { free(slots[k]); slots[k] = NULL; }
            else          { slots[k] = malloc(sz); *(volatile char*)slots[k] = 1; }
        }
        _mm_lfence();
        uint64_t t1 = __rdtsc();
        _mm_lfence();
        samples[r] = (t1 - t0);
    }
    for (int i = 0; i < 1024; ++i) if (slots[i]) free(slots[i]);
    free(slots);
    qsort(samples, reps, sizeof(uint64_t), cmp_u64);
    return (double)samples[reps/2] / (double)iters;
}

/* Pure alloc-then-free-all (bump + drain), no interleave. */
static double batch_cycles(size_t sz, int n, int reps) {
    uint64_t samples[64];
    if (reps > 64) reps = 64;
    void** slots = malloc((size_t)n * sizeof(void*));
    for (int i = 0; i < n; ++i) { void* p = malloc(sz); free(p); }  /* warm */
    for (int r = 0; r < reps; ++r) {
        _mm_lfence();
        uint64_t t0 = __rdtsc();
        for (int i = 0; i < n; ++i) { slots[i] = malloc(sz); *(volatile char*)slots[i] = 1; }
        for (int i = 0; i < n; ++i) { free(slots[i]); }
        uint64_t t1 = __rdtsc();
        _mm_lfence();
        samples[r] = (t1 - t0);
    }
    free(slots);
    qsort(samples, reps, sizeof(uint64_t), cmp_u64);
    return (double)samples[reps/2] / (double)(2 * n);  /* per malloc+free */
}

/* Isolate malloc: N slots pre-freed, time N mallocs (bin pops). */
static double malloc_only_cycles(size_t sz, int n, int reps) {
    uint64_t samples[64];
    if (reps > 64) reps = 64;
    void** slots = malloc((size_t)n * sizeof(void*));
    for (int i = 0; i < n; ++i) { void* p = malloc(sz); free(p); }  /* warm bin */
    for (int r = 0; r < reps; ++r) {
        for (int i = 0; i < n; ++i) slots[i] = malloc(sz);   /* drain bin */
        for (int i = 0; i < n; ++i) free(slots[i]);          /* refill bin */
        _mm_lfence();
        uint64_t t0 = __rdtsc();
        for (int i = 0; i < n; ++i) slots[i] = malloc(sz);   /* MEASURED */
        uint64_t t1 = __rdtsc();
        _mm_lfence();
        for (int i = 0; i < n; ++i) free(slots[i]);
        samples[r] = (t1 - t0);
    }
    free(slots);
    qsort(samples, reps, sizeof(uint64_t), cmp_u64);
    return (double)samples[reps/2] / (double)n;
}

/* Isolate free: N live blocks, time N frees (bin pushes). */
static double free_only_cycles(size_t sz, int n, int reps) {
    uint64_t samples[64];
    if (reps > 64) reps = 64;
    void** slots = malloc((size_t)n * sizeof(void*));
    for (int i = 0; i < n; ++i) { void* p = malloc(sz); free(p); }
    for (int r = 0; r < reps; ++r) {
        for (int i = 0; i < n; ++i) slots[i] = malloc(sz);
        _mm_lfence();
        uint64_t t0 = __rdtsc();
        for (int i = 0; i < n; ++i) free(slots[i]);          /* MEASURED */
        uint64_t t1 = __rdtsc();
        _mm_lfence();
        samples[r] = (t1 - t0);
    }
    free(slots);
    qsort(samples, reps, sizeof(uint64_t), cmp_u64);
    return (double)samples[reps/2] / (double)n;
}

int main(void) {
    printf("== %s cycle profile (median, lower=better) ==\n", NAME);
    printf("  churn  32B  : %6.2f cyc/op\n", churn_cycles(32, 4000000, 15));
    printf("  churn 256B  : %6.2f cyc/op\n", churn_cycles(256, 4000000, 15));
    printf("  malloc 32B  : %6.2f cyc/op (bin pop)\n", malloc_only_cycles(32, 4000, 5000));
    printf("  free   32B  : %6.2f cyc/op (bin push)\n", free_only_cycles(32, 4000, 5000));
    printf("  batch  32B  : %6.2f cyc/op (alloc+free)\n", batch_cycles(32, 100000, 15));
    return 0;
}
