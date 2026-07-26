/*
 * jetalloc — core allocator: thread heaps and the malloc/free fast paths.
 * SPDX-License-Identifier: MIT
 *
 * FAST PATHS (the whole point of the allocator):
 *   jet_malloc small  → heap->tcache[cls] pop, else active page pop (no atomics)
 *   jet_free   small   → if owner-thread: push onto heap->tcache[cls] (no atomics)
 *                        else: buffer into the outgoing remote cache, later
 *                              flushed to the OWNER HEAP's inbox with one CAS
 *                              per batch (snmalloc-style message passing)
 *
 * A page is entirely owner-private: cross-thread frees never touch it. They are
 * routed to the owning heap's inbox (remote_in) and folded back into the
 * owner's pages when it next drains on the alloc slow path. So the common
 * same-thread alloc/free loop stays in L1-resident owner-only state and the
 * cross-thread path pays zero atomics per object (one CAS per flushed batch).
 */
#include "jet_internal.h"
#include <string.h>
#include <stdlib.h>          /* getenv (JET_PLACE)                          */
#ifdef JET_DEBUG
#  include <stdio.h>
#  include <stdlib.h>
#endif

/* ── Thread heap registry ─────────────────────────────────────────────── */

/* The all-zero sentinel heap every thread starts on. Never written: the first
 * allocation finds every bin empty, falls into jet_malloc_refill, which sees the
 * sentinel and swaps in a real heap before touching anything. Letting threads
 * share it is therefore safe, and it buys the malloc fast path a removed
 * NULL-check (see jet_malloc_inline in jet_internal.h). */
jet_heap jet_null_heap;
_Thread_local jet_heap* jet_tls_heap = &jet_null_heap;
#define tls_heap jet_tls_heap

/* All heaps chained for teardown/trim. Guarded by a spinlock (cold). */
static _Atomic(jet_heap*) heap_registry = NULL;

static void remote_flush(jet_heap* h);   /* fwd: flush outgoing remote cache  */

#if !defined(_WIN32)
#  include <pthread.h>
/* A thread-exit destructor guarantees a dying thread posts any remote frees it
 * still has buffered (so cross-thread-freed memory is never stranded when the
 * freeing thread exits before hitting the flush threshold). The heap struct
 * itself is intentionally NOT torn down here — its pages may still hold live
 * blocks other threads will free later; it stays in the registry. */
static pthread_key_t  jet_tls_key;
static pthread_once_t jet_tls_once = PTHREAD_ONCE_INIT;
static void jet_thread_exit(void* arg) {
    jet_heap* h = (jet_heap*)arg;
    if (h) remote_flush(h);
}
static void jet_tls_init(void) { pthread_key_create(&jet_tls_key, jet_thread_exit); }

/* One-time per-CPU (rseq) subsystem init, guarded so it runs exactly once for
 * the whole process regardless of which thread allocates first. */
static pthread_once_t jet_percpu_once = PTHREAD_ONCE_INIT;
static void jet_percpu_bootstrap(void) { (void)jet_percpu_init(); jet_place_init(); }
#endif

/* Place-based / temperature-aware cross-thread free (experimental). Off unless
 * JET_PLACE is set non-zero. When off, the shipped message-passing engine runs
 * and jet_place_on stays 0 (one predicted branch on the free path). */
int jet_place_on = 0;
void jet_place_init(void) {
    const char* env = getenv("JET_PLACE");
    jet_place_on = (env && env[0] != '0' && env[0] != '\0') ? 1 : 0;
}

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
#if !defined(_WIN32)
    /* Register for the thread-exit flush. */
    pthread_once(&jet_tls_once, jet_tls_init);
    pthread_setspecific(jet_tls_key, h);
    /* Arm the per-CPU (rseq) fast path once for the process. */
    pthread_once(&jet_percpu_once, jet_percpu_bootstrap);
#endif
    /* Enrol this thread in the epoch (QSBR) reclamation scheme so its
     * quiescent states are visible to page reclaimers. */
    jet_epoch_register(h);
    return h;
}

jet_heap* jet_thread_heap(void) {
    jet_heap* h = tls_heap;
    /* &jet_null_heap is the pre-bootstrap sentinel, not a usable heap. */
    if (JET_LIKELY(h != &jet_null_heap)) return h;
    h = heap_create();
    /* On OOM keep the sentinel installed rather than NULL, so every fast path
     * that tests `!= &jet_null_heap` keeps routing to the slow path instead of
     * dereferencing NULL. Callers still see NULL returned and propagate it. */
    if (JET_UNLIKELY(h == NULL)) return NULL;
    tls_heap = h;
    return h;
}

/* ── Free-list plumbing ───────────────────────────────────────────────── */

/* Forward declarations for the cross-thread message-passing machinery, which
 * is defined below jet_free but referenced by the alloc slow path. */
static int  remote_drain(jet_heap* h);
static void free_to_page(jet_page* pg, void* ptr);

/* Move any recycled blocks (owner-side local_free) into alloc_free so the fast
 * pop path can serve them. Returns 1 if alloc_free is non-empty afterwards.
 * Does NOT touch the bump cursor — the caller bumps.
 *
 * In the default model cross-thread frees never reach a page directly. In the
 * PLACE-BASED model (JET_PLACE=1) they DO: they were pushed onto pg->place_head
 * by their address, without a used-- (the freer never touched page accounting).
 * So when we drain place_head we must decrement `used` per block to restore the
 * invariant "a block on alloc_free is not counted in used". One atomic exchange
 * grabs the whole remote stack. */
static int refill_page(jet_page* pg) {
    if (pg->alloc_free) return 1;
    void* reclaimed = pg->local_free;
    pg->local_free = NULL;

    if (jet_place_on) {
        void* remote = atomic_exchange_explicit(&pg->place_head, NULL,
                                                memory_order_acquire);
        while (remote) {
            void* nxt = *(void**)remote;
            *(void**)remote = reclaimed;   /* prepend onto reclaimed chain     */
            reclaimed = remote;
            pg->used--;                     /* restore accounting for this block */
            remote = nxt;
        }
    }
    pg->alloc_free = reclaimed;
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

/* Like page_pop, but reports whether the block is PRISTINE (came straight from
 * the bump region — never handed out, so still zero from the fresh mmap) vs
 * RECYCLED (from alloc_free — may hold stale bytes). Used by calloc to skip a
 * needless memset on pristine blocks. */
static JET_ALWAYS_INLINE void* page_pop_src(jet_page* pg, int* pristine) {
    void* b = pg->alloc_free;
    if (JET_LIKELY(b != NULL)) {
        pg->alloc_free = *(void**)b;
        pg->used++;
        *pristine = 0;                 /* recycled: caller must zero           */
        return b;
    }
    uint8_t* p = pg->bump;
    pg->bump = p + pg->block_size;
    pg->used++;
    /* Pristine (already zero) only if this page's memory is never-written mmap.
     * A RECYCLED page's bump region overlaps previously-written bytes. */
    *pristine = pg->mem_fresh;
    return p;
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

/* calloc's provenance-aware allocation of one block, reporting *pristine.
 * Mirrors alloc_small but uses page_pop_src so calloc can skip the memset when
 * the block came from a page's bump region (already zero from mmap). */
static void* alloc_small_src(jet_heap* h, int cls, int* pristine) {
    jet_page* pg = h->active[cls];
    if (JET_LIKELY(pg != NULL && page_has_room(pg)))
        return page_pop_src(pg, pristine);
    if (pg != NULL) {
        if (refill_page(pg))
            return page_pop_src(pg, pristine);
        pg->flags = JET_PG_FULL;
        h->active[cls] = NULL;
    }
    pg = obtain_page(h, cls);
    if (JET_UNLIKELY(!pg)) return NULL;
    return page_pop_src(pg, pristine);
}

/* Owner side of the place-based model: pop every page from our drain stack and
 * fold its accumulated place_head frees back in. Called on the alloc slow path
 * (a quiescent point). For each drained page we clear on_drain FIRST so a
 * concurrent freer that arrives after our exchange re-enqueues the page rather
 * than losing its free. Then we drain place_head via refill_page's accounting
 * and re-file the page: empty → epoch-retire, otherwise → partial (discoverable
 * for future allocs). Pages still ACTIVE/PARTIAL are left in place (their
 * place_head is drained lazily by refill_page anyway). */
static void place_drain_pages(jet_heap* h) {
    jet_page* pg = atomic_exchange_explicit(&h->drain_pages, NULL,
                                            memory_order_acquire);
    while (pg) {
        jet_page* nxt = atomic_load_explicit(&pg->drain_next, memory_order_relaxed);
        /* Release our claim before draining: a free racing in now re-enqueues. */
        atomic_store_explicit(&pg->on_drain, 0, memory_order_release);

        if (pg->flags == JET_PG_FULL) {
            /* Fold place frees into alloc_free (refill_page does the used--). */
            if (refill_page(pg)) {
                if (JET_UNLIKELY(pg->used == 0)) {
                    jet_epoch_retire(pg);
                } else {
                    partial_push(h, pg);   /* now has free blocks → discoverable */
                }
            }
        }
        /* ACTIVE/PARTIAL pages: leave them; refill_page drains place_head when
         * the owner next touches the page. */
        pg = nxt;
    }
}

/* Batch-refill: pull up to `want` blocks out of the heap's pages into the
 * class fast bin, then return one. Amortizes the alloc_small / page-header /
 * TLS overhead across a whole batch (transfer-cache idea, tcmalloc). Returns
 * NULL only on OOM. */
/* Batch-fill the tcache for `cls` from the active page. Blocks come out of the
 * page (bump ascending / alloc_free LIFO) and are prepended onto the bin. We
 * tried re-ordering the batch so mallocs return ascending addresses to court
 * the HW stride prefetcher — measured a NET LOSS (same-page blocks are already
 * cache-adjacent, and the reordering buffer cost real cycles), so we keep the
 * cheap direct prepend. */
static void tcache_batch_fill(jet_heap* h, int cls) {
    jet_page* pg = h->active[cls];
    if (JET_UNLIKELY(pg == NULL)) return;
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

static void* tcache_refill(jet_heap* h, int cls) {
    /* Alloc slow path: a natural quiescent point — we hold no foreign page
     * pointer here. Announce it so the epoch can advance and any limbo pages
     * that are now provably unreachable get reclaimed into the central pool. */
    jet_epoch_quiesce();
    /* Reclaim any cross-thread frees other threads posted to our inbox first,
     * so their memory re-enters our pages before we consider minting more. */
    if (JET_UNLIKELY(atomic_load_explicit(&h->remote_in, memory_order_relaxed)))
        remote_drain(h);
    /* Place-based model: fold back any pages remote threads freed into while
     * they were FULL (unreachable via our lists). */
    if (JET_UNLIKELY(jet_place_on &&
                     atomic_load_explicit(&h->drain_pages, memory_order_relaxed)))
        place_drain_pages(h);

    /* Grab the first block the normal way (also mints a page if needed). */
    void* first = alloc_small(h, cls);
    if (JET_UNLIKELY(!first)) return NULL;

    /* Then greedily pull more from the now-active page into the bin, ordered so
     * subsequent mallocs return ASCENDING addresses (prefetch-friendly). */
    tcache_batch_fill(h, cls);
    return first;
}

/* calloc's slow path: same prep + batch fill as tcache_refill, but returns the
 * first block's provenance so calloc can elide the memset for a pristine (bump)
 * block. The batch stuffed into the tcache is left untouched — those blocks go
 * through the normal recycled→zero path on a later calloc, which is correct
 * (only a missed optimisation, never stale data). */
static void* calloc_refill(jet_heap* h, int cls, int* pristine) {
    jet_epoch_quiesce();
    if (JET_UNLIKELY(atomic_load_explicit(&h->remote_in, memory_order_relaxed)))
        remote_drain(h);
    if (JET_UNLIKELY(jet_place_on &&
                     atomic_load_explicit(&h->drain_pages, memory_order_relaxed)))
        place_drain_pages(h);

    void* first = alloc_small_src(h, cls, pristine);
    if (JET_UNLIKELY(!first)) return NULL;

    tcache_batch_fill(h, cls);
    return first;
}

void* jet_malloc(size_t size) {
    JET_STAT_ADD(jet_stat_alloc_calls, 1);
    if (JET_UNLIKELY(size > JET_LARGE_THRESHOLD))
        return jet_large_alloc(size, JET_MIN_ALIGN);
    if (JET_UNLIKELY(size == 0)) size = 1;
    int cls = jet_size_class_fast(size);
    JET_STAT_ADD(jet_stat_live, jet_class_size[cls]);

    /* Inline the common thread-heap hit: a bare TLS load, no call. Only the
     * first allocation on a thread misses and falls into heap_create. */
    jet_heap* h = tls_heap;
    if (JET_LIKELY(h != &jet_null_heap)) {
        if (JET_LIKELY(!jet_percpu_on)) {
            void* b = h->tcache[cls];
            if (JET_LIKELY(b != NULL)) {
                h->tcache[cls] = *(void**)b;
                h->tcount[cls]--;
                return b;
            }
            return tcache_refill(h, cls);
        }
    } else {
        h = jet_thread_heap();
    }
    /* Per-CPU (rseq) fast path (opt-in via JET_PERCPU): pop from the current
     * CPU's slab with no atomics. Off by default — one predicted branch. */
    if (JET_UNLIKELY(jet_percpu_on)) {
        void* pc = jet_percpu_pop(cls);
        if (pc != NULL) return pc;
    }
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
    /* INVARIANT: free_to_page is only ever called for pages this heap owns.
     * Its two callers — tcache_flush (surplus of the owner's own frees) and
     * remote_drain (blocks other threads returned to OUR inbox, i.e. our
     * blocks) — both pass owner-owned pages. Cross-thread frees never reach a
     * page directly; they go through the heap inbox. So there is no per-page
     * atomic free list here at all. */
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
        /* Page fully empty and not active. Do NOT hand it straight back to
         * the central pool: a slow cross-thread freer may still hold this
         * pointer. Defer the real free to the epoch reclaimer, which only
         * repurposes the page once every thread has passed a quiescent state
         * (no stale pointer can survive). Unlink from our partial list first
         * — that's purely owner-local state, safe to touch now. */
        if (pg->flags == JET_PG_PARTIAL) partial_unlink(h, pg);
        jet_epoch_retire(pg);
        return;
    }
    if (pg->flags == JET_PG_FULL) {
        /* Was full, now has a free block again — make it discoverable. */
        partial_push(h, pg);
    }
    /* PARTIAL and still non-empty: already discoverable, nothing to do. */
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

/* Public wrappers so the inline drop-in fast paths (jet_internal.h, used by the
 * interposed malloc/free) reach the slow-path continuations across TU bounds. */
/* Public wrapper for the inline malloc fast path. `h` may be the all-zero
 * sentinel heap (thread not yet bootstrapped) — detect that and create the real
 * one first, which is exactly the old `h == NULL` slow path. */
void* jet_malloc_refill(jet_heap* h, int cls) {
    if (JET_UNLIKELY(h == &jet_null_heap)) {
        h = jet_thread_heap();
        if (JET_UNLIKELY(h == NULL)) return NULL;
    }
    return tcache_refill(h, cls);
}
void  jet_tcache_flush_pub(jet_heap* h, unsigned cls) { tcache_flush(h, cls); }

/* ── Cross-thread free: batched message passing (snmalloc-style) ───────── */

/* Push a whole chain [first..last] (count nodes) onto owner's inbox with ONE
 * atomic CAS. `last`'s link word is set to the previous inbox head so the
 * owner sees a single connected stack. This is the only atomic paid per
 * BATCH of remote frees — not per object. */
static void remote_post_batch(jet_heap* owner, void* first, void* last) {
    void* head = atomic_load_explicit(&owner->remote_in, memory_order_relaxed);
    do {
        *(void**)last = head;
    } while (!atomic_compare_exchange_weak_explicit(
        &owner->remote_in, &head, first, memory_order_release,
        memory_order_relaxed));
}

/* Flush all buffered outgoing remote frees: one batched CAS per target heap. */
static void remote_flush(jet_heap* h) {
    for (unsigned i = 0; i < JET_REMOTE_SLOTS; ++i) {
        jet_remote_bucket* b = &h->remote_out[i];
        if (b->owner) {
            remote_post_batch(b->owner, b->head, b->tail);
            b->owner = NULL; b->head = NULL; b->tail = NULL; b->count = 0;
        }
    }
    h->remote_pending = 0;
}

/* Buffer one cross-thread free into the outgoing cache, bucketed by target
 * heap. Pure local stores — no atomics on this path. Auto-flushes when the
 * cache fills. `cls` drives alloc-time local/shared classification: a class
 * seen freeing cross-thread repeatedly is marked shared-hot and its bucket is
 * flushed EAGERLY (small batch) so the owner recycles the memory promptly. */
static void remote_enqueue(jet_heap* h, jet_heap* owner, unsigned cls, void* ptr) {
    unsigned slot = (unsigned)(((uintptr_t)owner >> 6) & JET_REMOTE_MASK);
    jet_remote_bucket* b = &h->remote_out[slot];
    if (JET_UNLIKELY(b->owner && b->owner != owner)) {
        /* Slot collision with a different target: flush this one bucket now so
         * we never mix two owners' chains. */
        remote_post_batch(b->owner, b->head, b->tail);
        h->remote_pending -= b->count;
        b->owner = NULL; b->head = NULL; b->tail = NULL; b->count = 0;
    }
    if (!b->owner) { b->owner = owner; b->tail = ptr; }
    *(void**)ptr = b->head;   /* prepend; tail stays the first-inserted node  */
    b->head = ptr;
    b->count++;
    ++h->remote_pending;

    /* Classify: this class just took a cross-thread free — bump its shared
     * score (saturating). A confirmed shared-hot class earns a LARGER per-
     * bucket batch (fewer atomic posts per block); a rarely-shared class uses
     * a small batch so a stray cross-thread free can't strand blocks. */
    uint8_t sc = h->shared_score[cls];
    if (sc < 255) h->shared_score[cls] = sc + 1;
    if (JET_LIKELY(sc >= JET_SHARED_HOT)) {
        /* Confirmed shared-hot: accumulate a batch bounded by a fixed BYTE
         * budget (so small classes batch deeply, large classes flush promptly)
         * and bypass the global cross-class flush cap so a steady
         * producer/consumer stream costs the fewest possible atomics. */
        uint32_t cap = JET_SHARED_HOT_BYTES / jet_class_size[cls];
        if (cap < JET_SHARED_COLD) cap = JET_SHARED_COLD;
        if (JET_UNLIKELY(b->count >= cap)) {
            remote_post_batch(b->owner, b->head, b->tail);
            h->remote_pending -= b->count;
            b->owner = NULL; b->head = NULL; b->tail = NULL; b->count = 0;
        }
        return;
    }
    /* Rarely-shared class: small per-bucket cap, and still subject to the
     * global flush so a stray cross-thread free can't strand a block for long. */
    if (JET_UNLIKELY(b->count >= JET_SHARED_COLD)) {
        remote_post_batch(b->owner, b->head, b->tail);
        h->remote_pending -= b->count;
        b->owner = NULL; b->head = NULL; b->tail = NULL; b->count = 0;
        return;
    }
    if (JET_UNLIKELY(h->remote_pending >= JET_REMOTE_FLUSH))
        remote_flush(h);
}

/* Owner side: drain our inbox (one atomic-exchange grabs the whole stack) and
 * fold every returned block back into our pages. Called on the alloc slow
 * path so reclaimed cross-thread memory re-enters circulation. Returns the
 * number of blocks reclaimed. */
static int remote_drain(jet_heap* h) {
    void* chain = atomic_exchange_explicit(&h->remote_in, NULL,
                                           memory_order_acquire);
    int n = 0;
    while (chain) {
        void* next = *(void**)chain;
        /* These are our blocks (owner == h), possibly from many pages. Route
         * each to its page's owner-free path. */
        free_to_page(jet_page_of(chain), chain);
        chain = next;
        ++n;
    }
    return n;
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

    /* Per-CPU (rseq) fast path (opt-in via JET_PERCPU): return the block to the
     * current CPU's slab. Works regardless of the page's original owner — the
     * block is "checked out" until a later alloc pops it; page->used accounting
     * only moves when a block finally drains to free_to_page. Off by default. */
    if (JET_UNLIKELY(jet_percpu_on)) {
        if (jet_percpu_push((int)pg->cls, ptr)) return;
    }

    jet_heap* h = tls_heap;
    if (JET_LIKELY(h != &jet_null_heap) && JET_LIKELY(pg->owner == h)) {
        /* Owner-thread free: push onto the per-class fast bin. No page-header
         * bookkeeping, no flag machine — just two stores. No epoch pin: our
         * own page can't be retired-and-repurposed while WE are freeing into
         * it (only the owner retires, and that's us). A bare TLS load (no call)
         * thanks to initial-exec; only the first free on a thread misses. */
        unsigned cls = pg->cls;
        *(void**)ptr = h->tcache[cls];
        h->tcache[cls] = ptr;
        if (JET_UNLIKELY(++h->tcount[cls] > JET_TCACHE_MAX))
            tcache_flush(h, cls);
        return;
    }
    if (JET_UNLIKELY(h == &jet_null_heap)) h = jet_thread_heap();
    /* Cross-thread free. THIS is the hazardous path: the page could be retired
     * and repurposed by its owner while we route the block. Pin the epoch so
     * any concurrent retire is deferred until we leave, then re-read the owner
     * under the pin. If the page was repurposed to us in the meantime, treat it
     * as an owner free; otherwise enqueue to the (now pinned-stable) owner. The
     * pin cost lands only on cross-thread frees, never the owner fast path. */
    jet_epoch_enter();
    jet_heap* owner = pg->owner;
    if (JET_UNLIKELY(owner == h)) {
        jet_epoch_leave();
        unsigned cls = pg->cls;
        *(void**)ptr = h->tcache[cls];
        h->tcache[cls] = ptr;
        if (JET_UNLIKELY(++h->tcount[cls] > JET_TCACHE_MAX))
            tcache_flush(h, cls);
        return;
    }
    /* PLACE-BASED / TEMPERATURE-AWARE path (experimental, JET_PLACE=1).
     * Ownership doesn't route the block — its ADDRESS does. But we don't pay a
     * per-object atomic on every cross-thread free: a page must EARN it. Each
     * foreign free heats the page; while it is still HOT (few foreign frees) we
     * route through the cheap BATCHED message-passing engine (zero atomics per
     * free, one CAS per batch). Only once a page proves genuinely contended
     * (temp >= JET_TEMP_WARM) do cross-thread frees push straight onto its own
     * place_head — at which point the page-local MPSC + owner-drains-in-one-
     * exchange beats re-bucketing a hot stream. So HOT pages get batching's
     * throughput and WARM pages get place's routing-free directness. */
    if (JET_UNLIKELY(jet_place_on)) {
        uint8_t t = atomic_load_explicit(&pg->temp, memory_order_relaxed);
        if (t < JET_TEMP_MAX)
            atomic_store_explicit(&pg->temp, (uint8_t)(t + 1), memory_order_relaxed);
        if (t < JET_TEMP_WARM) {
            /* Still HOT: batch it (no per-object atomic). */
            remote_enqueue(h, owner, pg->cls, ptr);
            jet_epoch_leave();
            return;
        }
        /* WARM: place it by address — one atomic, no bucket, no owner routing. */
        void* head = atomic_load_explicit(&pg->place_head, memory_order_relaxed);
        do {
            *(void**)ptr = head;
        } while (!atomic_compare_exchange_weak_explicit(
            &pg->place_head, &head, ptr, memory_order_release,
            memory_order_relaxed));
        /* Make the owner able to FIND this page. active/partial pages are
         * reached on the owner's alloc path anyway, but a FULL page (dropped
         * from every list) would otherwise strand its place frees forever. So
         * enqueue the page on the owner's lock-free drain stack — exactly once
         * per outstanding burst, claimed via on_drain (0→1). The owner clears
         * on_drain when it drains, so a later free re-enqueues. This is one
         * atomic per page-transition, not per object. */
        if (atomic_exchange_explicit(&pg->on_drain, 1, memory_order_acq_rel) == 0) {
            jet_page* dh = atomic_load_explicit(&owner->drain_pages,
                                                memory_order_relaxed);
            do {
                atomic_store_explicit(&pg->drain_next, dh, memory_order_relaxed);
            } while (!atomic_compare_exchange_weak_explicit(
                &owner->drain_pages, &dh, pg, memory_order_release,
                memory_order_relaxed));
        }
        jet_epoch_leave();
        return;
    }
    remote_enqueue(h, owner, pg->cls, ptr);
    jet_epoch_leave();
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
    if (JET_UNLIKELY(total > JET_LARGE_THRESHOLD)) {
        /* Large allocs come straight from mmap, which is already zero-filled. */
        return jet_large_alloc(total, JET_MIN_ALIGN);
    }
    if (JET_UNLIKELY(total == 0)) total = 1;
    int cls = jet_size_class_fast(total);
    JET_STAT_ADD(jet_stat_alloc_calls, 1);
    JET_STAT_ADD(jet_stat_live, jet_class_size[cls]);

    /* calloc zero-elision (#14): a block taken from a page's BUMP region has
     * never been handed out before, so it is still zero from the fresh mmap —
     * memset is pure waste. Only RECYCLED blocks (tcache / alloc_free / a
     * remote free) may hold stale bytes and need clearing. We take the slow
     * path (which knows the block's provenance) and zero only when necessary.
     * The per-CPU path can't report provenance, so with JET_PERCPU on we fall
     * back to always-zero for correctness. */
    jet_heap* h = tls_heap;
    if (JET_LIKELY(h != &jet_null_heap && !jet_percpu_on)) {
        /* tcache blocks are recycled → must zero. */
        void* b = h->tcache[cls];
        if (b != NULL) {
            h->tcache[cls] = *(void**)b;
            h->tcount[cls]--;
            memset(b, 0, jet_class_size[cls]);
            return b;
        }
        /* Bin empty: refill, then the returned block may be pristine (bump) or
         * recycled. calloc_refill reports which so we skip the memset when we
         * safely can. */
        int pristine = 0;
        void* first = calloc_refill(h, cls, &pristine);
        if (JET_UNLIKELY(!first)) return NULL;
        if (!pristine) memset(first, 0, jet_class_size[cls]);
        return first;
    }
    /* Fallback (no heap yet, or per-CPU on): allocate then zero. */
    void* p = jet_malloc(total);
    if (p) memset(p, 0, jet_class_size[cls]);
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
    /* Fast path: a slab block lives inside the reserved arena — one range
     * compare, ZERO memory loads. This is the common case and what makes the
     * drop-in free() competitive with mimalloc/tcmalloc. */
    if (JET_LIKELY(jet_arena_contains(ptr))) {
        /* Page-aligned pointers inside the arena would be page headers, never
         * user blocks, so a genuine slab user pointer is never page-aligned. */
        return ((uintptr_t)ptr & (JET_PAGE_SIZE - 1)) != 0;
    }
    /* Page-aligned pointer outside the arena: possibly a large (direct-mmap)
     * allocation of ours — validate via its header magic. */
    if (((uintptr_t)ptr & (JET_PAGE_SIZE - 1)) == 0)
        return jet_large_owns(ptr);
    /* Arena disabled (reservation failed) fallback: sniff the page header. */
    if (jet_arena_disabled()) {
        jet_page* pg = jet_page_of(ptr);
        return pg->block_size >= 8 && pg->block_size <= JET_LARGE_THRESHOLD &&
               pg->capacity > 0 && pg->cls < JET_NUM_CLASSES;
    }
    return 0;   /* not page-aligned, not in arena, arena on → foreign */
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
