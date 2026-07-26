/*
 * jetalloc — core allocator: thread heaps and the malloc/free fast paths.
 * SPDX-License-Identifier: MIT
 *
 * FAST PATHS (the whole point of the allocator):
 *   jet_malloc small  → heap->active[cls]->alloc_free pop  (no atomics, no lock)
 *   jet_free   small   → if owner-thread: push onto page->local_free (no atomics)
 *                        else: atomic Treiber push onto page->thread_free
 *
 * The active-page free list (alloc_free) is refilled lazily: when it empties we
 * (1) steal the page's own local_free, (2) drain its atomic thread_free, and
 * only if still empty do we go find/mint another page. This keeps the common
 * same-thread alloc/free loop entirely in the L1-resident page header.
 */
#include "jet_internal.h"
#include <string.h>
#ifdef JET_DEBUG
#  include <stdio.h>
#  include <stdlib.h>
#endif

/* ── Thread heap registry ─────────────────────────────────────────────── */

static _Thread_local jet_heap* tls_heap = NULL;

/* All heaps chained for teardown/trim. Guarded by a spinlock (cold). */
static _Atomic(jet_heap*) heap_registry = NULL;

static jet_heap* heap_create(void) {
    /* Bootstrap the heap struct itself from a dedicated large alloc so we do
     * not recurse through jet_malloc before a heap exists. */
    jet_heap* h = (jet_heap*)jet_large_alloc(sizeof(jet_heap), JET_PAGE_SIZE);
    if (!h) return NULL;
    memset(h, 0, sizeof(*h));
    /* Push onto the registry (lock-free). */
    jet_heap* head = atomic_load_explicit(&heap_registry, memory_order_relaxed);
    do {
        h->next_heap = head;
    } while (!atomic_compare_exchange_weak_explicit(
        &heap_registry, &head, h, memory_order_release, memory_order_relaxed));
    return h;
}

jet_heap* jet_thread_heap(void) {
    jet_heap* h = tls_heap;
    if (JET_LIKELY(h != NULL)) return h;
    h = heap_create();
    tls_heap = h;
    return h;
}

/* ── Free-list plumbing ───────────────────────────────────────────────── */

/* Drain a page's atomic cross-thread free list into a plain pointer chain,
 * returning the head. Lock-free: single atomic exchange grabs the whole stack. */
static inline void* drain_thread_free(jet_page* pg) {
    return atomic_exchange_explicit(&pg->thread_free, NULL,
                                    memory_order_acquire);
}

/* Concatenate list `b` onto the tail is O(n); instead we just prepend, since
 * ordering within a page's free list is irrelevant to correctness. */
static inline void* splice(void* head, void* other) {
    if (!other) return head;
    if (!head)  return other;
    /* walk `other` to its tail, link to head. `other` lists are short (one
     * refill's worth), so this is cheap and keeps everything intrusive. */
    void* p = other;
    while (*(void**)p) p = *(void**)p;
    *(void**)p = head;
    return other;
}

/* Move any recycled blocks (owner local_free + cross-thread thread_free) into
 * alloc_free so the fast pop path can serve them. Returns 1 if alloc_free is
 * non-empty afterwards. Does NOT touch the bump cursor — the caller bumps. */
static int refill_page(jet_page* pg) {
    if (pg->alloc_free) return 1;
    void* reclaimed = pg->local_free;
    pg->local_free = NULL;
    void* remote = drain_thread_free(pg);
    pg->alloc_free = splice(reclaimed, remote);
    return pg->alloc_free != NULL;
}

/* Page states — a page is in EXACTLY one of these at all times, and its
 * membership in a heap list is implied by the state:
 *   ACTIVE  : it is heap->active[cls]           (not in any linked list)
 *   PARTIAL : linked in heap->partial[cls]       (has free blocks, not active)
 *   FULL    : no free blocks, not active         (not in any linked list)
 * FREE pages (used==0, not active) are returned to the central pool. */
#define JET_PG_ACTIVE  1u
#define JET_PG_PARTIAL 2u
#define JET_PG_FULL    3u

static inline void partial_unlink(jet_heap* h, jet_page* pg) {
    if (pg->prev) pg->prev->next = pg->next;
    else          h->partial[pg->cls] = pg->next;
    if (pg->next) pg->next->prev = pg->prev;
    pg->prev = pg->next = NULL;
}

static inline void partial_push(jet_heap* h, jet_page* pg) {
    pg->prev = NULL;
    pg->next = h->partial[pg->cls];
    if (pg->next) pg->next->prev = pg;
    h->partial[pg->cls] = pg;
    pg->flags = JET_PG_PARTIAL;
}

/* Find or create a page with a free block for class `cls`, install it as the
 * active page, and return it. NULL on OOM. */
static jet_page* obtain_page(jet_heap* h, int cls) {
    /* Try partial pages first. A PARTIAL page only ever holds recycled blocks
     * (its bump region was consumed before it was demoted), so refill_page is
     * the right test. Promote the first one that yields a block. */
    jet_page* pg = h->partial[cls];
    while (pg) {
        jet_page* nxt = pg->next;
        if (refill_page(pg)) {
            partial_unlink(h, pg);
            pg->flags = JET_PG_ACTIVE;
            h->active[cls] = pg;
            return pg;
        }
        pg = nxt;
    }
    /* Mint a fresh page — it starts with a full bump region (page_has_room). */
    pg = jet_central_fresh_page(h, cls);
    if (!pg) return NULL;
    pg->flags = JET_PG_ACTIVE;
    h->active[cls] = pg;
    return pg;
}

/* ── malloc ───────────────────────────────────────────────────────────── */

/* Pop one block from a page that is known to have capacity (alloc_free
 * non-empty OR bump < bump_end). Pure fast path — no list scans. */
static JET_ALWAYS_INLINE void* page_pop(jet_page* pg) {
    void* b = pg->alloc_free;
    if (JET_LIKELY(b != NULL)) {
        pg->alloc_free = *(void**)b;   /* recycled block: hot, already resident */
        pg->used++;
        return b;
    }
    /* Bump: hand out a never-yet-touched block by pure arithmetic. */
    uint8_t* p = pg->bump;
    pg->bump = p + pg->block_size;
    pg->used++;
    return p;
}

/* Does this page have ANY servable block right now (recycled or bumpable)? */
static JET_ALWAYS_INLINE int page_has_room(jet_page* pg) {
    return pg->alloc_free != NULL || pg->bump < pg->bump_end;
}

static void* alloc_small(jet_heap* h, int cls) {
    jet_page* pg = h->active[cls];
    if (JET_LIKELY(pg != NULL && page_has_room(pg)))
        return page_pop(pg);

    if (pg != NULL) {
        /* Active page exhausted its recycled list AND its bump region. Try to
         * reclaim deferred/cross-thread frees in place before giving up on it. */
        if (refill_page(pg))
            return page_pop(pg);
        /* Genuinely full: mark FULL, drop from active. */
        pg->flags = JET_PG_FULL;
        h->active[cls] = NULL;
    }
    pg = obtain_page(h, cls);
    if (JET_UNLIKELY(!pg)) return NULL;
    return page_pop(pg);
}

/* Batch-refill: pull up to `want` blocks out of the heap's pages into the
 * class fast bin, then return one. Amortizes the alloc_small / page-header /
 * TLS overhead across a whole batch (transfer-cache idea, tcmalloc). Returns
 * NULL only on OOM. */
static void* tcache_refill(jet_heap* h, int cls) {
    /* Grab the first block the normal way (also mints a page if needed). */
    void* first = alloc_small(h, cls);
    if (JET_UNLIKELY(!first)) return NULL;

    /* Then greedily pull more from the now-active page straight into the bin,
     * without re-entering alloc_small per block. Cap the batch so a single
     * malloc can't front-load an unbounded amount of work. */
    jet_page* pg = h->active[cls];
    if (JET_LIKELY(pg != NULL)) {
        uint32_t want = JET_TCACHE_MAX / 2;
        void* head = h->tcache[cls];
        uint32_t got = 0;
        while (got < want && page_has_room(pg)) {
            void* b = page_pop(pg);
            *(void**)b = head;
            head = b;
            ++got;
        }
        h->tcache[cls] = head;
        h->tcount[cls] += got;
    }
    return first;
}

void* jet_malloc(size_t size) {
    JET_STAT_ADD(jet_stat_alloc_calls, 1);
    if (JET_UNLIKELY(size > JET_LARGE_THRESHOLD))
        return jet_large_alloc(size, JET_MIN_ALIGN);
    int cls = jet_size_class(size);
    JET_STAT_ADD(jet_stat_live, jet_class_size[cls]);

    jet_heap* h = jet_thread_heap();
    /* Fast bin hit: pop a recycled block without touching any page header. */
    void* b = h->tcache[cls];
    if (JET_LIKELY(b != NULL)) {
        h->tcache[cls] = *(void**)b;
        h->tcount[cls]--;
        return b;
    }
    /* Bin empty: refill a batch from pages, amortizing the slow path. */
    return tcache_refill(h, cls);
}

/* ── free ─────────────────────────────────────────────────────────────── */

static void free_to_page(jet_page* pg, void* ptr) {
    jet_heap* h = jet_thread_heap();
    if (JET_LIKELY(pg->owner == h)) {
        /* Owner-thread free: push to local_free, no atomics. */
        *(void**)ptr = pg->local_free;
        pg->local_free = ptr;
        pg->used--;

        if (pg->flags == JET_PG_ACTIVE) {
            /* Active page: block goes straight back via local_free; the next
             * alloc miss reclaims it. Nothing to relink. Do NOT retire even if
             * used==0 — it stays the active page. */
            return;
        }
        if (JET_UNLIKELY(pg->used == 0)) {
            /* Page fully empty and not active — return it to the central pool. */
            if (pg->flags == JET_PG_PARTIAL) partial_unlink(h, pg);
            jet_central_retire_page(pg);
            return;
        }
        if (pg->flags == JET_PG_FULL) {
            /* Was full, now has a free block again — make it discoverable. */
            partial_push(h, pg);
        }
        /* PARTIAL and still non-empty: already discoverable, nothing to do. */
    } else {
        /* Cross-thread free: atomic Treiber push onto the owner's thread_free. */
        void* head = atomic_load_explicit(&pg->thread_free, memory_order_relaxed);
        do {
            *(void**)ptr = head;
        } while (!atomic_compare_exchange_weak_explicit(
            &pg->thread_free, &head, ptr, memory_order_release,
            memory_order_relaxed));
    }
}

/* Flush surplus fast-bin blocks for class `cls` back to their owning pages.
 * Called when a bin exceeds JET_TCACHE_MAX. We keep the MRU half (still hot in
 * cache, likely to be reallocated soon) and return the LRU half to pages via
 * free_to_page — which does the used-- bookkeeping and retires empty pages, so
 * memory is actually reclaimable. Blocks in a bin are all the same class but
 * may belong to different pages; free_to_page routes each correctly. */
static void tcache_flush(jet_heap* h, unsigned cls) {
    void* node = h->tcache[cls];
    uint32_t keep = JET_TCACHE_MAX / 2;
    /* Walk to the split point, keeping the first `keep` nodes cached. */
    for (uint32_t i = 1; i < keep && node; ++i)
        node = *(void**)node;
    if (!node) { h->tcount[cls] = h->tcount[cls] < keep ? h->tcount[cls] : keep; return; }
    void* surplus = *(void**)node;
    *(void**)node = NULL;            /* terminate the kept list              */
    h->tcount[cls] = keep;
    /* Return the surplus chain to pages. */
    while (surplus) {
        void* next = *(void**)surplus;
        free_to_page(jet_page_of(surplus), surplus);
        surplus = next;
    }
}

void jet_free(void* ptr) {
    if (JET_UNLIKELY(ptr == NULL)) return;
    JET_STAT_ADD(jet_stat_free_calls, 1);
    /* Large (page-aligned) allocations are handled by their own registry. */
    if (JET_UNLIKELY(((uintptr_t)ptr & (JET_PAGE_SIZE - 1)) == 0)) {
        if (jet_large_free(ptr)) return;
        /* page-aligned but not ours → foreign; ignore (interposer safety). */
        return;
    }
    jet_page* pg = jet_page_of(ptr);
    JET_STAT_SUB(jet_stat_live, pg->block_size);

    jet_heap* h = jet_thread_heap();
    if (JET_LIKELY(pg->owner == h)) {
        /* Owner-thread free: push onto the per-class fast bin. No page-header
         * bookkeeping, no flag machine — just two stores. */
        unsigned cls = pg->cls;
        *(void**)ptr = h->tcache[cls];
        h->tcache[cls] = ptr;
        if (JET_UNLIKELY(++h->tcount[cls] > JET_TCACHE_MAX))
            tcache_flush(h, cls);
        return;
    }
    /* Cross-thread free: hand the block back to its owning page's lock-free
     * remote-free stack (the owner drains it on its next refill). */
    void* head = atomic_load_explicit(&pg->thread_free, memory_order_relaxed);
    do {
        *(void**)ptr = head;
    } while (!atomic_compare_exchange_weak_explicit(
        &pg->thread_free, &head, ptr, memory_order_release,
        memory_order_relaxed));
}

void jet_free_sized(void* ptr, size_t size) {
    (void)size;  /* size lets us skip the class lookup; page header already
                  * carries block_size so the saving is marginal here, but the
                  * symbol exists for C++ sized-delete to bind to. */
    jet_free(ptr);
}

/* ── calloc / realloc / aligned ───────────────────────────────────────── */

void* jet_calloc(size_t count, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(count, size, &total)) return NULL;
    void* p = jet_malloc(total);
    if (!p) return NULL;
    /* Fresh OS pages are already zero; only slab-reused memory needs clearing.
     * We conservatively zero small allocs (reused) and rely on mmap zero for
     * large ones. */
    if (total <= JET_LARGE_THRESHOLD) memset(p, 0, total);
    return p;
}

size_t jet_usable_size(const void* ptr) {
    if (!ptr) return 0;
    if (((uintptr_t)ptr & (JET_PAGE_SIZE - 1)) == 0) {
        size_t u = jet_large_usable(ptr);
        if (u) return u;
        return 0;
    }
    if (!jet_owns(ptr)) return 0;
    return jet_page_of(ptr)->block_size;
}

void* jet_realloc(void* ptr, size_t size) {
    if (!ptr) return jet_malloc(size);
    if (size == 0) { jet_free(ptr); return NULL; }
    size_t cur = jet_usable_size(ptr);
    if (cur >= size && (size > JET_LARGE_THRESHOLD || cur <= 2 * size)) {
        /* Shrink or same class — keep it, avoids churn (only when not wildly
         * over-sized, to actually give memory back on big shrinks). */
        if (cur >= size && size > JET_LARGE_THRESHOLD) return ptr;
        if (cur >= size) return ptr;
    }
    void* np = jet_malloc(size);
    if (!np) return NULL;
    memcpy(np, ptr, cur < size ? cur : size);
    jet_free(ptr);
    return np;
}

void* jet_aligned_alloc(size_t alignment, size_t size) {
    if (alignment <= JET_MIN_ALIGN) return jet_malloc(size);
    /* Any alignment a slab can't natively guarantee goes through the large
     * (page-aligned mmap) path. Page-aligned covers every power-of-two
     * alignment up to JET_PAGE_SIZE and beyond, so the result is always
     * correctly aligned. Slabs only guarantee JET_MIN_ALIGN, so we do NOT try
     * to satisfy 32/64/.../4096-byte alignment from them — correctness first. */
    return jet_large_alloc(size, alignment < JET_PAGE_SIZE ? JET_PAGE_SIZE
                                                           : alignment);
}

int jet_posix_memalign(void** out, size_t alignment, size_t size) {
    if (alignment < sizeof(void*) || (alignment & (alignment - 1)))
        return 22 /* EINVAL */;
    void* p = jet_aligned_alloc(alignment, size);
    if (!p) return 12 /* ENOMEM */;
    *out = p;
    return 0;
}

/* ── introspection ────────────────────────────────────────────────────── */

int jet_owns(const void* ptr) {
    if (!ptr) return 0;
    if (((uintptr_t)ptr & (JET_PAGE_SIZE - 1)) == 0)
        return jet_large_owns(ptr);
    /* Slab block: its page header must have a plausible block_size / capacity.
     * We can't fully validate without a span registry, but the aligned-large
     * check above plus this sanity gate rejects most foreign pointers. A full
     * span bitmap is a future hardening step. */
    jet_page* pg = jet_page_of(ptr);
    return pg->block_size >= 8 && pg->block_size <= JET_LARGE_THRESHOLD &&
           pg->capacity > 0 && pg->cls < JET_NUM_CLASSES;
}

void jet_get_stats(jet_stats* out) {
    if (!out) return;
    out->bytes_mapped = atomic_load_explicit(&jet_stat_mapped, memory_order_relaxed);
    out->bytes_live   = atomic_load_explicit(&jet_stat_live, memory_order_relaxed);
    out->pages_active = atomic_load_explicit(&jet_stat_pages, memory_order_relaxed);
    out->large_active = atomic_load_explicit(&jet_stat_large, memory_order_relaxed);
    out->alloc_calls  = atomic_load_explicit(&jet_stat_alloc_calls, memory_order_relaxed);
    out->free_calls   = atomic_load_explicit(&jet_stat_free_calls, memory_order_relaxed);
}

void jet_trim(void) { /* empty pages already returned eagerly; reserved. */ }

const char* jet_version(void) { return "0.1.0"; }
