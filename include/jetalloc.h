/*
 * jetalloc — a fast, modern, drop-in memory allocator.
 *
 * Public C API. Include this to call the allocator explicitly (jet_malloc,
 * jet_free, ...). For transparent replacement of the system allocator, just
 * link libjetalloc — it interposes malloc/free/calloc/realloc and the C++
 * operator new/delete family (see src/jet_override.c / src/jet_override.cpp).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef JETALLOC_H
#define JETALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  define JET_EXPORT __declspec(dllexport)
#else
#  define JET_EXPORT __attribute__((visibility("default")))
#endif

/* Attribute helpers for callers that want the optimizer to know our
 * malloc-family functions return non-aliasing, freshly-allocated memory. */
#if defined(__GNUC__) || defined(__clang__)
#  define JET_MALLOC_ATTR __attribute__((malloc))
#  define JET_ALLOC_SIZE(i) __attribute__((alloc_size(i)))
#  define JET_ALLOC_SIZE2(i, j) __attribute__((alloc_size(i, j)))
#else
#  define JET_MALLOC_ATTR
#  define JET_ALLOC_SIZE(i)
#  define JET_ALLOC_SIZE2(i, j)
#endif

/* ── Core API ─────────────────────────────────────────────────────────── */

JET_EXPORT void* jet_malloc(size_t size) JET_MALLOC_ATTR JET_ALLOC_SIZE(1);
JET_EXPORT void  jet_free(void* ptr);
JET_EXPORT void* jet_calloc(size_t count, size_t size)
    JET_MALLOC_ATTR JET_ALLOC_SIZE2(1, 2);
JET_EXPORT void* jet_realloc(void* ptr, size_t size) JET_ALLOC_SIZE(2);

/* Aligned allocation. `alignment` must be a power of two. */
JET_EXPORT void* jet_aligned_alloc(size_t alignment, size_t size)
    JET_MALLOC_ATTR JET_ALLOC_SIZE(2);
JET_EXPORT int   jet_posix_memalign(void** out, size_t alignment, size_t size);

/* Sized free — the fast path. If the caller knows the request size (e.g. C++
 * sized operator delete), pass it to skip the page-metadata size lookup. */
JET_EXPORT void  jet_free_sized(void* ptr, size_t size);

/* Usable size of an allocation (like malloc_usable_size). Returns 0 for a
 * pointer jetalloc did not allocate — this is how the interposer stays safe. */
JET_EXPORT size_t jet_usable_size(const void* ptr);

/* True iff `ptr` was handed out by jetalloc. Constant-time. Used by the
 * malloc/free interposer so a free() of a pointer from a DIFFERENT allocator
 * (e.g. a libc buffer created before we loaded) is routed away instead of
 * corrupting our heap or crashing. */
JET_EXPORT int   jet_owns(const void* ptr);

/* ── Introspection / lifecycle ────────────────────────────────────────── */

typedef struct jet_stats {
    size_t bytes_mapped;      /* total virtual memory reserved from the OS  */
    size_t bytes_live;        /* bytes currently handed out to callers      */
    size_t pages_active;      /* live 64 KiB slab pages                     */
    size_t large_active;      /* live large (direct-mmap) allocations       */
    size_t alloc_calls;       /* lifetime jet_malloc-family calls           */
    size_t free_calls;        /* lifetime jet_free-family calls             */
} jet_stats;

JET_EXPORT void jet_get_stats(jet_stats* out);

/* Release empty slab pages / large caches back to the OS. Safe to call any
 * time; a no-op if there is nothing to reclaim. */
JET_EXPORT void jet_trim(void);

/* Version string, e.g. "0.1.0". */
JET_EXPORT const char* jet_version(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* JETALLOC_H */
