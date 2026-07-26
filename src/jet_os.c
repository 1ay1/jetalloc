/* jetalloc — OS memory primitives. SPDX-License-Identifier: MIT */
#include "jet_internal.h"

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <unistd.h>
#  ifndef MAP_ANONYMOUS
#    define MAP_ANONYMOUS MAP_ANON
#  endif
#endif

_Atomic(size_t) jet_stat_mapped      = 0;
_Atomic(size_t) jet_stat_live        = 0;
_Atomic(size_t) jet_stat_pages       = 0;
_Atomic(size_t) jet_stat_large       = 0;
_Atomic(size_t) jet_stat_alloc_calls = 0;
_Atomic(size_t) jet_stat_free_calls  = 0;

/* ── Reserved slab arena (address-range ownership) ────────────────────────
 * The KILLER cost of a drop-in malloc is `free(ptr)` having to decide whether
 * ptr is ours: a page-header load on every free (cold-line miss) is what the
 * fastest allocators avoid. We copy snmalloc/mimalloc: reserve ONE huge
 * contiguous virtual range up front (PROT_NONE, no RAM until touched) and carve
 * every slab span from it. Then `jet_owns_slab(ptr)` is a single unsigned
 * compare — `(ptr - base) < reserved` — with ZERO memory loads. On 64-bit we
 * reserve 64 GiB of address space (free; the kernel commits lazily on touch).
 * If the reservation fails (32-bit, ASLR pressure, overcommit off) we fall back
 * to per-span mmap and the slower header-sniff owns(). */
#if !defined(_WIN32) && defined(__LP64__)
#  define JET_ARENA_BYTES  ((size_t)64 * 1024 * 1024 * 1024)   /* 64 GiB VA     */
/* The arena base, published for the INLINE single-compare ownership test in
 * jet_internal.h. Sentinel for "no arena": UINTPTR_MAX, chosen so the unsigned
 * wrap trick (p - base < JET_ARENA_BYTES) is false for every real pointer
 * without needing a separate base!=0 branch. That turns free()'s ownership test
 * into ONE load + sub + cmp-immediate instead of two global loads and two
 * compares. */
uintptr_t jet_arena_base_pub = UINTPTR_MAX;
static _Atomic(uintptr_t) g_arena_base = 0;   /* 0 = arena disabled            */
static uintptr_t          g_arena_end  = 0;   /* base + reserved (const once)  */
static _Atomic(uintptr_t) g_arena_cur  = 0;   /* bump cursor (atomic)          */

/* Reserve the arena once. Idempotent; safe under races (first winner keeps it).*/
static void arena_reserve(void) {
    if (atomic_load_explicit(&g_arena_base, memory_order_acquire)) return;
    void* p = mmap(NULL, JET_ARENA_BYTES, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return;                       /* fall back to per-span */
    uintptr_t base = (uintptr_t)p;
    uintptr_t expect = 0;
    if (atomic_compare_exchange_strong_explicit(
            &g_arena_base, &expect, base,
            memory_order_acq_rel, memory_order_acquire)) {
        g_arena_end = base + JET_ARENA_BYTES;
        atomic_store_explicit(&g_arena_cur, base, memory_order_release);
        /* Publish LAST: until this store lands the inline test sees the
         * UINTPTR_MAX sentinel and simply reports "not ours", which routes the
         * pointer down the safe header-sniff path. Never a false positive. */
        __atomic_store_n(&jet_arena_base_pub, base, __ATOMIC_RELEASE);
    } else {
        munmap(p, JET_ARENA_BYTES);                    /* lost the race         */
    }
}

/* Carve an aligned span from the arena and commit it R/W. NULL if the arena is
 * off or exhausted (caller falls back to standalone mmap). */
static void* arena_carve(size_t bytes, size_t align) {
    uintptr_t base = atomic_load_explicit(&g_arena_base, memory_order_acquire);
    if (!base) return NULL;
    for (;;) {
        uintptr_t cur = atomic_load_explicit(&g_arena_cur, memory_order_relaxed);
        uintptr_t aligned = (cur + align - 1) & ~(align - 1);
        uintptr_t next = aligned + bytes;
        if (next > g_arena_end) return NULL;           /* arena full            */
        if (!atomic_compare_exchange_weak_explicit(
                &g_arena_cur, &cur, next,
                memory_order_acq_rel, memory_order_relaxed))
            continue;
        /* Commit the slice R/W over the reserved range (MAP_FIXED replaces the
         * PROT_NONE placeholder in place). */
        void* got = mmap((void*)aligned, bytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (got == MAP_FAILED) return NULL;
        return (void*)aligned;
    }
}

/* Single-compare ownership test: is this a slab pointer we carved? See the
 * inline JET_ARENA_CONTAINS in jet_internal.h — this out-of-line form exists for
 * the non-inlined callers and mirrors it exactly. */
int jet_arena_contains(const void* ptr) {
    uintptr_t base = __atomic_load_n(&jet_arena_base_pub, __ATOMIC_ACQUIRE);
    return (uintptr_t)ptr - base < JET_ARENA_BYTES;
}

/* 1 iff the arena is NOT active (reservation failed) — callers then fall back to
 * the header-sniff ownership test. We consider it "active" once a reserve has
 * been attempted AND succeeded; before first span map it's not yet reserved, so
 * report disabled=1 only after an attempt. Simpler: report base==0. */
int jet_arena_disabled(void) {
    return atomic_load_explicit(&g_arena_base, memory_order_acquire) == 0;
}
#else
uintptr_t jet_arena_base_pub = UINTPTR_MAX;
int jet_arena_contains(const void* ptr) { (void)ptr; return 0; }
int jet_arena_disabled(void) { return 1; }
#endif

void* jet_os_map(size_t bytes) {
    return jet_os_map_aligned(bytes, JET_PAGE_SIZE);
}

/*
 * Reserve `bytes` of zero-filled memory aligned to `align` (a power of two,
 * >= page granularity). Strategy: over-map by `align`, then trim the head and
 * tail slack so the payload is aligned without a second syscall dance on the
 * common path.
 */
void* jet_os_map_aligned(size_t bytes, size_t align) {
#if defined(_WIN32)
    /* VirtualAlloc granularity is 64 KiB; over-reserve and re-map aligned. */
    if (align <= 65536) {
        void* p = VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE,
                               PAGE_READWRITE);
        if (p) atomic_fetch_add_explicit(&jet_stat_mapped, bytes,
                                         memory_order_relaxed);
        return p;
    }
    /* Aligned > 64 KiB: reserve, free, re-commit at aligned base (racy on
     * Windows but large aligns are rare; acceptable for now). */
    for (;;) {
        void* probe = VirtualAlloc(NULL, bytes + align, MEM_RESERVE,
                                   PAGE_NOACCESS);
        if (!probe) return NULL;
        uintptr_t base = ((uintptr_t)probe + align - 1) & ~(align - 1);
        VirtualFree(probe, 0, MEM_RELEASE);
        void* p = VirtualAlloc((void*)base, bytes, MEM_COMMIT | MEM_RESERVE,
                               PAGE_READWRITE);
        if (p) {
            atomic_fetch_add_explicit(&jet_stat_mapped, bytes,
                                      memory_order_relaxed);
            return p;
        }
        /* Lost the race — retry. */
    }
#else
    size_t over = bytes + align;
    void* raw = mmap(NULL, over, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) return NULL;

    uintptr_t base    = (uintptr_t)raw;
    uintptr_t aligned = (base + align - 1) & ~(align - 1);

    size_t head = aligned - base;
    size_t tail = over - head - bytes;
    if (head) munmap((void*)base, head);
    if (tail) munmap((void*)(aligned + bytes), tail);

#if defined(MADV_HUGEPAGE)
    /* Ask the kernel to back this region with transparent huge pages when it
     * can: fewer page faults, smaller page tables, far better TLB hit rate on
     * the 2 MiB/4 MiB slab spans. Advisory — ignored if THP is off. */
    if (bytes >= (2u << 20))
        madvise((void*)aligned, bytes, MADV_HUGEPAGE);
#endif

    /* NUMA: prefer the local node for this span so first-touch places its
     * physical pages in the allocating thread's local DRAM. No-op (one branch,
     * no syscall) on single-node machines. */
    jet_numa_bind_local((void*)aligned, bytes);

    atomic_fetch_add_explicit(&jet_stat_mapped, bytes, memory_order_relaxed);
    return (void*)aligned;
#endif
}

/* Map a SLAB span. Prefers the reserved contiguous arena so that every slab
 * block later resolves ownership by a single range compare (jet_arena_contains)
 * with no memory load. Falls back to a standalone aligned mmap if the arena is
 * unavailable or full. Large direct allocations deliberately do NOT use this —
 * they munmap individually on free, which would punch holes in the arena. */
void* jet_os_map_span(size_t bytes, size_t align) {
#if !defined(_WIN32) && defined(__LP64__)
    arena_reserve();
    void* acar = arena_carve(bytes, align);
    if (acar) {
#if defined(MADV_HUGEPAGE)
        if (bytes >= (2u << 20)) madvise(acar, bytes, MADV_HUGEPAGE);
#endif
        jet_numa_bind_local(acar, bytes);
        atomic_fetch_add_explicit(&jet_stat_mapped, bytes, memory_order_relaxed);
        return acar;
    }
#endif
    return jet_os_map_aligned(bytes, align);   /* fallback: standalone mmap */
}

void jet_os_unmap(void* p, size_t bytes) {
    if (!p) return;
#if defined(_WIN32)
    (void)bytes;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, bytes);
#endif
    atomic_fetch_sub_explicit(&jet_stat_mapped, bytes, memory_order_relaxed);
}
