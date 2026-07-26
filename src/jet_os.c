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

    atomic_fetch_add_explicit(&jet_stat_mapped, bytes, memory_order_relaxed);
    return (void*)aligned;
#endif
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
