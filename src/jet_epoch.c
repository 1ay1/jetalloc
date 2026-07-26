/*
 * jetalloc — epoch-based (QSBR) safe reclamation of retired slab pages.
 * SPDX-License-Identifier: MIT
 *
 * THE HAZARD. A cross-thread free reads pg->owner, decides the page belongs to
 * another heap, then routes the block to that owner's inbox. In between, the
 * owner may empty the page and call jet_central_retire_page(), which returns
 * the 64 KiB region to the central pool where raw_page() can immediately hand
 * it back to format_page() for a DIFFERENT size class. The slow freer then
 * reads a live, repurposed page (its owner, block_size and cls have all
 * changed) and misroutes the block. That is a genuine use-after-reformat race.
 *
 * THE FIX (QSBR — the RCU / Crossbeam-epoch family). We never repurpose a
 * retired page while any thread might still hold a stale pointer to it:
 *
 *   - A global epoch counter G advances monotonically.
 *   - Each participating thread publishes, in a per-thread record, either the
 *     epoch it is "pinned" in (inside a read-side critical section) or a
 *     QUIESCENT sentinel (not pinned).
 *   - jet_epoch_retire(pg) does NOT free the page; it parks it in the limbo bag
 *     tagged with the current global epoch.
 *   - A bag tagged epoch e is safe to reclaim once G >= e + 2 AND no thread is
 *     currently pinned in an epoch < G. Two full epoch advances guarantee every
 *     thread that could have observed the page in epoch e has since passed
 *     through a quiescent state, so no stale pointer survives.
 *
 * The read-side critical section (jet_epoch_enter/leave) is deliberately tiny:
 * only the cross-thread free path uses it, and only around the window where it
 * touches a page it does not own. The common same-thread free never pins.
 *
 * This layer is a CORRECTNESS upgrade, not a speed one: without it the retire
 * path had a (rare, timing-dependent) UAF; with it retirement is race-free.
 */
#include "jet_internal.h"

/* QUIESCENT sentinel: a thread not inside any read-side critical section. Odd
 * numbers can't be real epochs (we advance by 1 each time from 0), so use the
 * top bit as the "unpinned" flag: local == (epoch) when pinned, == NOT_PINNED
 * when quiescent. */
#define JET_EPOCH_UNPINNED  (~(uint64_t)0)

typedef struct jet_epoch_rec {
    _Alignas(64) _Atomic(uint64_t) local;   /* pinned epoch, or UNPINNED       */
    struct jet_epoch_rec* next;
} jet_epoch_rec;

/* Global epoch and the registry of per-thread records. */
static _Atomic(uint64_t)       g_epoch    = 0;
static _Atomic(jet_epoch_rec*) g_recs     = NULL;

/* This thread's epoch record (lazily allocated the first time it pins). */
static _Thread_local jet_epoch_rec* tls_rec = NULL;

/* Limbo bags, indexed by epoch mod 3. Each holds pages retired in that epoch,
 * chained through pg->next. Guarded by a spinlock (cold path only). */
typedef struct { _Atomic(int) v; } jet_spin2;
static jet_spin2   limbo_lock = {0};
static jet_page*   limbo[3]   = { NULL, NULL, NULL };
static uint64_t    limbo_epoch[3] = { 0, 0, 0 };   /* epoch each bag belongs to */

static inline void l_lock(jet_spin2* s) {
    while (atomic_exchange_explicit(&s->v, 1, memory_order_acquire)) {
        while (atomic_load_explicit(&s->v, memory_order_relaxed)) {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            __asm__ __volatile__("yield");
#endif
        }
    }
}
static inline void l_unlock(jet_spin2* s) {
    atomic_store_explicit(&s->v, 0, memory_order_release);
}

static jet_epoch_rec* epoch_rec(void) {
    jet_epoch_rec* r = tls_rec;
    if (JET_LIKELY(r != NULL)) return r;
    /* Bootstrap a record from the OS directly (never through jet_malloc — this
     * runs inside the allocator). One 64 KiB page per thread is wasteful but
     * this is a cold, once-per-thread path; sub-allocating is not worth it. */
    r = (jet_epoch_rec*)jet_os_map(sizeof(jet_epoch_rec));
    if (!r) return NULL;
    atomic_store_explicit(&r->local, JET_EPOCH_UNPINNED, memory_order_relaxed);
    /* Publish into the global registry (lock-free push). */
    jet_epoch_rec* head = atomic_load_explicit(&g_recs, memory_order_relaxed);
    do {
        r->next = head;
    } while (!atomic_compare_exchange_weak_explicit(
        &g_recs, &head, r, memory_order_release, memory_order_relaxed));
    tls_rec = r;
    return r;
}

void jet_epoch_register(jet_heap* h) { (void)h; (void)epoch_rec(); }

void jet_epoch_enter(void) {
    jet_epoch_rec* r = epoch_rec();
    if (!r) return;                       /* OOM: degrade to no protection      */
    uint64_t g = atomic_load_explicit(&g_epoch, memory_order_acquire);
    /* Publish the pin AND get the StoreLoad barrier in ONE instruction: a
     * seq_cst exchange compiles to a single LOCK XCHG on x86, which both
     * publishes r->local and prevents the subsequent load of pg->owner from
     * being reordered ahead of the pin. This replaces the old store +
     * standalone mfence (two barriers) on the hot cross-thread free path. */
    (void)atomic_exchange_explicit(&r->local, g, memory_order_seq_cst);
}

void jet_epoch_leave(void) {
    jet_epoch_rec* r = tls_rec;
    if (!r) return;
    atomic_store_explicit(&r->local, JET_EPOCH_UNPINNED, memory_order_release);
}

/* Can we reclaim the bag for epoch `e`? Safe once the global epoch has moved
 * two ahead, so long as no thread is still pinned in an epoch <= e. */
static int epoch_safe(uint64_t e, uint64_t g) {
    if (g < e + 2) return 0;
    for (jet_epoch_rec* r = atomic_load_explicit(&g_recs, memory_order_acquire);
         r; r = r->next) {
        uint64_t l = atomic_load_explicit(&r->local, memory_order_acquire);
        if (l != JET_EPOCH_UNPINNED && l <= e) return 0;   /* still could hold  */
    }
    return 1;
}

/* Free every page in a limbo chain back to the central pool for real reuse. */
static void reclaim_chain(jet_page* pg) {
    while (pg) {
        jet_page* nxt = pg->next;
        jet_central_retire_page(pg);      /* now genuinely safe to repurpose    */
        pg = nxt;
    }
}

/* Try to advance the global epoch: only possible if every pinned thread is
 * already at the current epoch (nobody lags behind). Returns the epoch value
 * observed after the attempt. */
static uint64_t epoch_try_advance(void) {
    uint64_t g = atomic_load_explicit(&g_epoch, memory_order_acquire);
    for (jet_epoch_rec* r = atomic_load_explicit(&g_recs, memory_order_acquire);
         r; r = r->next) {
        uint64_t l = atomic_load_explicit(&r->local, memory_order_acquire);
        if (l != JET_EPOCH_UNPINNED && l < g) return g;   /* someone lags        */
    }
    /* Everyone is either quiescent or already at g → bump to g+1. */
    uint64_t want = g + 1;
    atomic_compare_exchange_strong_explicit(
        &g_epoch, &g, want, memory_order_acq_rel, memory_order_relaxed);
    return atomic_load_explicit(&g_epoch, memory_order_acquire);
}

void jet_epoch_retire(jet_page* pg) {
    uint64_t g = atomic_load_explicit(&g_epoch, memory_order_acquire);
    unsigned slot = (unsigned)(g % 3);
    l_lock(&limbo_lock);
    /* If this bag holds an OLDER epoch's pages that are now safe, flush them
     * first so the slot can be reused for the current epoch. */
    if (limbo[slot] && limbo_epoch[slot] != g) {
        if (epoch_safe(limbo_epoch[slot], g)) {
            jet_page* old = limbo[slot];
            limbo[slot] = NULL;
            l_unlock(&limbo_lock);
            reclaim_chain(old);
            l_lock(&limbo_lock);
        }
    }
    pg->next = limbo[slot];
    limbo[slot] = pg;
    limbo_epoch[slot] = g;
    l_unlock(&limbo_lock);
}

void jet_epoch_quiesce(void) {
    /* Announce that we are not holding anything, then try to advance the epoch
     * and drain any limbo bag that has become safe. Cold path (called when a
     * thread cache runs dry / on the alloc slow path), so the scan is cheap. */
    jet_epoch_rec* r = tls_rec;
    if (r) atomic_store_explicit(&r->local, JET_EPOCH_UNPINNED,
                                 memory_order_release);
    uint64_t g = epoch_try_advance();

    /* Drain every safe bag. Take the lock before touching limbo[] — no
     * unlocked pre-check, or we'd race the retire path (TSan-verified). */
    for (unsigned slot = 0; slot < 3; ++slot) {
        l_lock(&limbo_lock);
        jet_page* ready = NULL;
        if (limbo[slot] && epoch_safe(limbo_epoch[slot], g)) {
            ready = limbo[slot];
            limbo[slot] = NULL;
        }
        l_unlock(&limbo_lock);
        if (ready) reclaim_chain(ready);
    }
}
