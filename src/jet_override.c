/*
 * jetalloc — C malloc-family interposition.
 * SPDX-License-Identifier: MIT
 *
 * Defines the standard names (malloc/free/calloc/realloc/...) so that linking
 * libjetalloc (or LD_PRELOAD-ing it) transparently replaces the system
 * allocator. Safety: free()/realloc() consult jet_owns() and forward pointers
 * jetalloc did NOT allocate to the underlying libc allocator. This is what
 * makes replacement crash-proof even when libc handed out buffers before we
 * were initialised (the exact failure mode that plagues naive free() overrides).
 */
#include "jet_internal.h"
#include <string.h>

#if !defined(_WIN32)
#  include <dlfcn.h>
#endif

/* Pointers to the real libc allocator, resolved lazily via dlsym. Used only
 * for foreign frees; jetalloc never routes NEW allocations here. */
typedef void* (*real_malloc_t)(size_t);
typedef void  (*real_free_t)(void*);
typedef void* (*real_realloc_t)(void*, size_t);

static real_free_t    real_free    = NULL;
static real_realloc_t real_realloc = NULL;

static void resolve_reals(void) {
#if !defined(_WIN32)
    if (!real_free)    real_free    = (real_free_t)dlsym(RTLD_NEXT, "free");
    if (!real_realloc) real_realloc = (real_realloc_t)dlsym(RTLD_NEXT, "realloc");
#endif
}

/* Visibility: the standard names must be globally visible + not inlined away. */
#define JET_PUBLIC __attribute__((visibility("default"), used))

JET_PUBLIC void* malloc(size_t size) { return jet_malloc_inline(size); }

JET_PUBLIC void free(void* ptr) {
    /* Owner-fast bin push inlines here (leaf); everything else defers. */
    if (JET_LIKELY(jet_free_inline(ptr))) return;
    /* Slow / foreign: full path decides ours-vs-libc and cross-thread. */
    if (JET_LIKELY(jet_owns(ptr))) { jet_free(ptr); return; }
    resolve_reals();
    if (real_free) real_free(ptr);
}

JET_PUBLIC void* calloc(size_t n, size_t sz) { return jet_calloc(n, sz); }

JET_PUBLIC void* realloc(void* ptr, size_t size) {
    if (!ptr) return jet_malloc(size);
    if (JET_LIKELY(jet_owns(ptr))) return jet_realloc(ptr, size);
    resolve_reals();
    if (real_realloc) return real_realloc(ptr, size);
    /* Last resort: copy out of the foreign block is impossible without its
     * size; hand back a fresh block (data loss avoided by the dlsym path above
     * in practice). */
    return jet_malloc(size);
}

JET_PUBLIC void* aligned_alloc(size_t alignment, size_t size) {
    return jet_aligned_alloc(alignment, size);
}

JET_PUBLIC int posix_memalign(void** out, size_t alignment, size_t size) {
    return jet_posix_memalign(out, alignment, size);
}

JET_PUBLIC void* memalign(size_t alignment, size_t size) {
    return jet_aligned_alloc(alignment, size);
}

JET_PUBLIC void* valloc(size_t size) {
    return jet_aligned_alloc(JET_PAGE_SIZE, size);
}

/* glibc/BSD extensions some programs call. */
JET_PUBLIC size_t malloc_usable_size(void* ptr) {
    return jet_owns(ptr) ? jet_usable_size(ptr) : 0;
}

JET_PUBLIC void cfree(void* ptr) { free(ptr); }
