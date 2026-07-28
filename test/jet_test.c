/* jetalloc — C correctness tests. SPDX-License-Identifier: MIT */
#include "jetalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#endif

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

static void test_basic(void) {
    void* p = jet_malloc(100);
    CHECK(p != NULL);
    CHECK(jet_owns(p));
    CHECK(jet_usable_size(p) >= 100);
    memset(p, 0xAB, 100);
    jet_free(p);
}

static void test_all_size_classes(void) {
    /* Alloc across the small ladder and the large threshold; write a canary
     * spanning the whole usable region to catch class-size / overlap bugs. */
    for (size_t sz = 1; sz <= 128 * 1024; sz = sz + (sz / 3) + 1) {
        void* p = jet_malloc(sz);
        CHECK(p != NULL);
        CHECK(jet_usable_size(p) >= sz);
        memset(p, 0x5A, sz);
        /* verify no corruption of the byte just written */
        CHECK(((unsigned char*)p)[sz - 1] == 0x5A);
        jet_free(p);
    }
}

static void test_alignment(void) {
    /* Sub-page alignments over the small ladder. */
    size_t aligns[] = {16, 32, 64, 128, 256, 4096, 65536};
    for (size_t i = 0; i < sizeof(aligns)/sizeof(aligns[0]); ++i) {
        for (size_t sz = 1; sz <= 9000; sz = sz * 2 + 1) {
            void* p = jet_aligned_alloc(aligns[i], sz);
            CHECK(p != NULL);
            CHECK(((uintptr_t)p & (aligns[i] - 1)) == 0);
            jet_free(p);
        }
    }
    /* OVER-page alignments (> 64 KiB) hit the large/direct-mmap path, which
     * must over-map so the payload is truly `align`-aligned — not merely
     * page-aligned. Regression guard for the aligned_alloc bug where any
     * alignment > 64 KiB silently returned only page-aligned memory. Write a
     * canary across the whole reported usable region to catch OOB too. */
    size_t big_aligns[] = {131072, 262144, 524288, 1u<<20, 2u<<20, 4u<<20};
    for (size_t i = 0; i < sizeof(big_aligns)/sizeof(big_aligns[0]); ++i) {
        size_t a = big_aligns[i];
        for (size_t sz = 1; sz <= 200000; sz = sz * 8 + 1) {
            void* p = jet_aligned_alloc(a, sz);
            CHECK(p != NULL);
            CHECK(((uintptr_t)p & (a - 1)) == 0);   /* truly align-aligned  */
            CHECK(jet_usable_size(p) >= sz);
            memset(p, 0xDA, sz);                     /* full payload writable */
            CHECK(((unsigned char*)p)[sz - 1] == 0xDA);
            jet_free(p);
        }
    }
    /* posix_memalign with a huge alignment. */
    void* q = NULL;
    CHECK(jet_posix_memalign(&q, 1u << 20, 5000) == 0);
    CHECK(q != NULL && ((uintptr_t)q & ((1u << 20) - 1)) == 0);
    jet_free(q);
}

static void test_calloc_zeroed(void) {
    for (size_t sz = 1; sz <= 4096; sz *= 2) {
        unsigned char* p = (unsigned char*)jet_calloc(1, sz);
        CHECK(p != NULL);
        for (size_t i = 0; i < sz; ++i) CHECK(p[i] == 0);
        jet_free(p);
    }
    volatile size_t huge = (size_t)-1;
    CHECK(jet_calloc(huge, 2) == NULL);  /* overflow guarded */
}

static void test_realloc(void) {
    char* p = (char*)jet_malloc(16);
    strcpy(p, "hello");
    p = (char*)jet_realloc(p, 4096);
    CHECK(p != NULL);
    CHECK(strcmp(p, "hello") == 0);       /* data preserved on grow */
    p = (char*)jet_realloc(p, 8);
    CHECK(p != NULL);
    CHECK(strncmp(p, "hello", 5) == 0);   /* data preserved on shrink */
    jet_free(p);
    CHECK(jet_realloc(NULL, 32) != NULL); /* realloc(NULL) == malloc */
}

static void test_churn(void) {
    /* Allocate/free the same class repeatedly — the page free list must recycle
     * blocks without unbounded growth. */
    enum { N = 4096 };
    void* ptrs[N];
    for (int round = 0; round < 50; ++round) {
        for (int i = 0; i < N; ++i) {
            ptrs[i] = jet_malloc(64);
            CHECK(ptrs[i] != NULL);
            *(volatile char*)ptrs[i] = (char)i;
        }
        for (int i = 0; i < N; ++i) jet_free(ptrs[i]);
    }
}

static void test_distinct_pointers(void) {
    /* No two live allocations may alias. */
    enum { N = 2000 };
    void* ptrs[N];
    for (int i = 0; i < N; ++i) {
        ptrs[i] = jet_malloc(48);
        for (int j = 0; j < i; ++j) CHECK(ptrs[i] != ptrs[j]);
    }
    for (int i = 0; i < N; ++i) jet_free(ptrs[i]);
}

static void test_foreign_safe(void) {
    /* jet_owns() is the interposer's safety gate: on ANY pointer we did not
     * allocate it must answer "not ours" WITHOUT dereferencing it (a foreign
     * deref segfaults, or a false positive routes a foreign block into jet_free
     * → corruption). Throw a spread of genuinely foreign pointers at it. */
    int stack_var = 7;
    CHECK(jet_owns(&stack_var) == 0);      /* stack — not ours              */
    CHECK(jet_usable_size(&stack_var) == 0);
    jet_free(NULL);                         /* free(NULL) is a no-op         */
    /* NB: we do NOT call jet_free(&stack_var). Like libc free(), jet_free()'s
     * contract is "a pointer THIS allocator returned" — feeding it foreign
     * memory is UB. The drop-in interposer (jet_override.*) is what makes
     * free()/delete safe on foreign pointers, and it does so by screening with
     * jet_owns() first — which is exactly what the checks in this test cover. */

    /* A fresh, independent mmap region jetalloc never handed out. Both a
     * page-aligned and an interior (non-aligned) pointer must be disowned
     * without reading the region. This is the case that crashed under a
     * disabled arena before the span-registry range check. */
#if defined(__unix__) || defined(__APPLE__)
    void* region = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region != MAP_FAILED) {
        CHECK(jet_owns(region) == 0);             /* page-aligned foreign  */
        CHECK(jet_owns((char*)region + 17) == 0); /* interior foreign      */
        CHECK(jet_usable_size(region) == 0);
        munmap(region, 4096);
    }
#endif

    /* A low, obviously-invalid pointer must also be safely disowned. */
    CHECK(jet_owns((void*)0x1234) == 0);
}

int main(void) {
    printf("jetalloc %s — C correctness tests\n", jet_version());
    test_basic();
    test_all_size_classes();
    test_alignment();
    test_calloc_zeroed();
    test_realloc();
    test_churn();
    test_distinct_pointers();
    test_foreign_safe();

    jet_stats s;
    jet_get_stats(&s);
    printf("stats: live=%zu mapped=%zu pages=%zu large=%zu alloc=%zu free=%zu\n",
           s.bytes_live, s.bytes_mapped, s.pages_active, s.large_active,
           s.alloc_calls, s.free_calls);

    if (fails) { printf("\n%d CHECK(s) FAILED\n", fails); return 1; }
    printf("\nALL PASSED\n");
    return 0;
}
