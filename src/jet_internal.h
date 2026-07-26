/*
 * jetalloc internal definitions — size classes, page layout, thread heap.
 * Not installed; consumed only by the src/ translation units.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef JETALLOC_INTERNAL_H
#define JETALLOC_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "jetalloc.h"

/* ── Tunables ─────────────────────────────────────────────────────────── */

/* Every slab page is exactly this size and this-aligned. A block's owning
 * Page* is recovered by masking its address with ~(JET_PAGE_SIZE-1), so blocks
 * carry no individual header. 64 KiB balances TLB pressure vs. per-page waste. */
#define JET_PAGE_SIZE      (64u * 1024u)
#define JET_PAGE_MASK      (~((uintptr_t)JET_PAGE_SIZE - 1))

/* Slab pages are carved from larger OS spans to amortise mmap syscalls. */
#define JET_SPAN_SIZE      (4u * 1024u * 1024u)   /* 4 MiB = 64 pages       */

/* Requests larger than this bypass slabs and go straight to mmap. */
#define JET_LARGE_THRESHOLD (32u * 1024u)

/* Minimum alignment guaranteed by jet_malloc (matches max_align_t on x86-64,
 * aarch64 — 16 bytes — so we satisfy the C standard without over-aligning). */
#define JET_MIN_ALIGN      16u

/* Number of segregated size classes (see jet_size_class table). */
#define JET_NUM_CLASSES    39

/* ── Size classes ─────────────────────────────────────────────────────── */

/* Byte size of each class, and a fast size→class index map for the small
 * range. Defined in jet_size.c. The ladder keeps internal fragmentation
 * <= 12.5% for every class boundary. */
extern const uint32_t jet_class_size[JET_NUM_CLASSES];

/* Map a (small) request size to its class index in O(1). Callers must have
 * already checked size <= JET_LARGE_THRESHOLD. */
int jet_size_class(size_t size);

/* ── Page (slab) header ───────────────────────────────────────────────── */

/*
 * One Page governs a single 64 KiB region serving ONE size class.
 *
 * Free-list sharding (the mimalloc trick): a page keeps two owner-local free
 * lists.
 *   - local_free: blocks the owner freed but hasn't yet folded back into the
 *     bump/reuse list. Popped/pushed with no atomics.
 *   - alloc_free: the reuse list the fast path pops from; when it empties the
 *     owner refills it from local_free. This split lets `free` be a branchless
 *     push and `malloc` a branchless pop in the common same-thread case.
 *
 * There is deliberately NO per-page cross-thread free list. Cross-thread frees
 * never touch the page — they are batched into the OWNER HEAP's inbox
 * (jet_heap.remote_in) via snmalloc-style message passing, so a page header is
 * entirely owner-private and fits in a single cache line (no _Alignas(64)
 * contended tail, no false sharing to design around). The owner folds inbox
 * blocks back through the normal owner-free path when it drains.
 */
typedef struct jet_page {
    /* ---- hot: touched on the alloc fast path (keep in the first cache line) */
    void*             alloc_free;     /* recycled-block pop list (hot)         */
    uint8_t*          bump;           /* bump cursor: never-yet-used region    */
    uint8_t*          bump_end;       /* one past the last bumpable block      */
    uint32_t          block_size;     /* bytes per block (a class size)        */
    uint32_t          used;           /* blocks currently allocated            */
    /* ---- warm: free bookkeeping + list membership (owner-only)              */
    void*             local_free;     /* owner-thread deferred frees           */
    struct jet_page*  next;           /* intrusive: partial/full page lists    */
    struct jet_page*  prev;
    struct jet_heap*  owner;          /* heap that owns this page              */
    uint32_t          capacity;       /* total blocks in the page              */
    uint16_t          cls;            /* size-class index                      */
    uint16_t          flags;          /* JET_PG_* state (see jet_core.c)       */
} jet_page;

/* ── Per-thread heap ──────────────────────────────────────────────────── */

/* Thread-cache depth: how many recycled blocks per class we hold in the heap's
 * fast bins before flushing surplus back to their pages. Bounds cache memory
 * while keeping the alloc/free hot path off the page headers most of the time. */
#define JET_TCACHE_MAX 64

/* ── Cross-thread (remote) free: batched message passing ──────────────────
 * A remote free (this thread frees a block owned by ANOTHER thread's heap)
 * does NOT touch the owner per object. Instead each thread accumulates remote
 * frees in a local RemoteCache, bucketed by target heap, and periodically
 * FLUSHES each bucket as ONE batch onto the owner's lock-free inbox with a
 * single atomic. The owner drains its whole inbox in one atomic-exchange on
 * its slow path. This is snmalloc's message-passing scheme: thousands of
 * cross-thread frees cost one atomic per batch, not one CAS per object. */
#define JET_REMOTE_SLOTS   16      /* outgoing buckets (power of two)          */
#define JET_REMOTE_MASK    (JET_REMOTE_SLOTS - 1)
#define JET_REMOTE_FLUSH   256     /* pending remote frees before auto-flush   */

/* Alloc-time local/shared classification thresholds. A class whose shared_score
 * saturates at/above JET_SHARED_HOT is confirmed producer/consumer traffic. We
 * exploit that by letting its outgoing bucket accumulate a big batch before
 * flushing — more blocks per atomic post = fewer CASes on the owner's inbox =
 * markedly higher cross-thread throughput (measured ~1.4× on the prod/cons
 * bench). The batch is capped by BYTES (JET_SHARED_HOT_BYTES), not a fixed
 * count, so small classes batch deeply (16 B → thousands of blocks) while large
 * classes flush promptly (32 KiB → a handful) and stranded memory stays
 * bounded. A rarely-shared class uses a small fixed count so a one-off
 * cross-thread free can't strand a block. */
#define JET_SHARED_HOT       8         /* score at which a class is shared-hot */
#define JET_SHARED_HOT_BYTES (512u*1024u) /* per-bucket byte budget, shared-hot */
#define JET_SHARED_COLD      24        /* per-bucket flush count, rarely-shared */

typedef struct jet_remote_bucket {
    struct jet_heap* owner;   /* target heap for this bucket (NULL = empty)    */
    void*            head;    /* intrusive chain of pending blocks             */
    void*            tail;    /* tail, so a batch splices onto the inbox O(1)  */
    uint32_t         count;   /* blocks queued in this bucket                  */
} jet_remote_bucket;

typedef struct jet_heap {
    /* Fast bins: per-class LIFO of recycled blocks. Alloc pops here first,
     * free pushes here first — both without touching a page header. Mirrors
     * glibc's tcache / mimalloc's thread free list. */
    void*       tcache[JET_NUM_CLASSES];
    uint32_t    tcount[JET_NUM_CLASSES];
    /* One "current" page per size class for O(1) refill/flush. */
    jet_page*   active[JET_NUM_CLASSES];
    /* Partially-free pages per class, tried when `active` fills. */
    jet_page*   partial[JET_NUM_CLASSES];
    /* Outgoing remote-free cache: blocks this thread freed that belong to
     * OTHER heaps, bucketed by target, flushed in batches. */
    jet_remote_bucket remote_out[JET_REMOTE_SLOTS];
    uint32_t          remote_pending;   /* total across all buckets           */
    /* Alloc-time local/shared classification (scalloc/mimalloc idea): a
     * saturating per-class score of how "shared" (cross-thread freed) each
     * size class is for this thread. Classes that turn out to be
     * producer/consumer heavy get their outgoing batches flushed EAGERLY so
     * the owner recycles them fast; purely-local classes never flush at all.
     * Cheap: one byte per class, bumped on the (already-cold) cross-thread
     * free path, read on the same path — the owner fast path never touches it. */
    uint8_t           shared_score[JET_NUM_CLASSES];
    struct jet_heap* next_heap;        /* global registry for teardown        */
    uint64_t    tid;                   /* owning thread id (debug)            */
    /* Incoming inbox: batches of our blocks freed by OTHER threads land here
     * via a single atomic push. On its own cache line — remote producers CAS
     * this while we read our hot fields above. */
    _Alignas(64) _Atomic(void*) remote_in;
} jet_heap;

/* Access the calling thread's heap (creates on first touch). */
jet_heap* jet_thread_heap(void);

/* ── Per-CPU cache (rseq) ─────────────────────────────────────────────────
 * A restartable-sequence per-CPU LIFO sits IN FRONT of the per-thread tcache.
 * On Linux/x86-64 with rseq(2) (glibc auto-registers it), alloc/free hit the
 * slab owned by whatever CPU the thread runs on — no atomics, no locks, and no
 * per-thread cache-line ping-pong when many threads share few cores. This is
 * tcmalloc's per-CPU cache mechanism. Falls back transparently to the tcache
 * when rseq is unavailable (old kernel, non-x86-64, opt-out). */
#define JET_PERCPU_MAX     128     /* blocks per (cpu,class) before overflow   */

/* Fast inline guard read by the malloc/free hot path so that, when the per-CPU
 * cache is disabled (the default), the only cost is a predicted-not-taken
 * branch on a hot global — no function call at all. Written once at init. */
extern int jet_percpu_on;

/* Initialise the per-CPU subsystem once. Returns 1 if rseq is live and the
 * per-CPU fast path is armed, 0 if we must fall back to the tcache. */
int   jet_percpu_init(void);
/* 1 if the per-CPU fast path is active for this process. */
int   jet_percpu_active(void);
/* Fast path: pop/push a block of class `cls` from the current CPU's slab.
 * pop returns NULL when that slab is empty (caller falls back to tcache).
 * push returns 1 on success, 0 when the slab is full (caller keeps the block
 * in the tcache / returns it to its page). */
void* jet_percpu_pop(int cls);
int   jet_percpu_push(int cls, void* block);

/* rseq asm primitives (jet_rseq.S). Present only on x86-64 Linux. */
#if defined(__x86_64__) && defined(__linux__)
void* jet_rseq_pop (void* base, long stride, long cls_off, long rseq_off);
int   jet_rseq_push(void* base, long stride, long cls_off, long rseq_off,
                    void* block);
#endif

/* ── Central span pool (locked, cold) ─────────────────────────────────── */

jet_page* jet_central_fresh_page(jet_heap* h, int cls);  /* new formatted page */
void      jet_central_retire_page(jet_page* pg);          /* fully-empty page   */

/* ── Epoch-based (QSBR) safe page reclamation ─────────────────────────────
 * A retired page must not be REPURPOSED (re-minted for a different class or
 * unmapped) while another thread still holds a stale pointer to it and is about
 * to route a block through it. Such a thread is inside a "read-side critical
 * section": between reading pg->owner and committing the cross-thread route.
 *
 * We solve this with quiescent-state-based reclamation (QSBR), the same class
 * of algorithm as Linux RCU / the Crossbeam epoch crate: retire_page parks the
 * page in a LIMBO list tagged with the current global epoch instead of freeing
 * it immediately. A page is only truly freed once the global epoch has
 * advanced far enough that every thread has passed through a quiescent state
 * since the page was retired — at which point no thread can still hold the
 * stale pointer. Threads announce quiescence with jet_epoch_quiesce() on their
 * cold path; the cross-thread free path brackets its hazardous window with
 * jet_epoch_enter()/jet_epoch_leave(). */
void jet_epoch_register(jet_heap* h);     /* enrol a heap in the epoch scheme  */
void jet_epoch_enter(void);               /* begin a read-side critical section */
void jet_epoch_leave(void);               /* end it (announces a quiescent pt)  */
void jet_epoch_retire(jet_page* pg);      /* defer page free until safe         */
void jet_epoch_quiesce(void);             /* cold-path: try to advance + reclaim */

/* ── Large (direct-mmap) allocations ──────────────────────────────────── */

void*  jet_large_alloc(size_t size, size_t align);
int    jet_large_free(void* ptr);          /* 1 if it was a large alloc      */
size_t jet_large_usable(const void* ptr);  /* 0 if not a large alloc         */
int    jet_large_owns(const void* ptr);

/* ── OS memory primitives (jet_os.c) ──────────────────────────────────── */

void* jet_os_map(size_t bytes);            /* JET_PAGE_SIZE-aligned reserve   */
void* jet_os_map_aligned(size_t bytes, size_t align);
void  jet_os_unmap(void* p, size_t bytes);

/* ── NUMA / topology-aware placement (jet_numa.c) ─────────────────────────
 * Bind freshly-mapped spans to the local NUMA node so a thread's allocations
 * live in its local DRAM. Entirely self-disabling on single-node machines (a
 * predicted branch, no syscall) and best-effort (MPOL_PREFERRED) elsewhere. */
void jet_numa_init(void);                  /* detect topology once             */
int  jet_numa_active(void);                /* 1 iff binding is armed (>1 node) */
void jet_numa_bind_local(void* addr, size_t len);  /* prefer caller's node     */

/* ── Global stat counters (relaxed atomics) ───────────────────────────── */

extern _Atomic(size_t) jet_stat_mapped;
extern _Atomic(size_t) jet_stat_live;
extern _Atomic(size_t) jet_stat_pages;
extern _Atomic(size_t) jet_stat_large;
extern _Atomic(size_t) jet_stat_alloc_calls;
extern _Atomic(size_t) jet_stat_free_calls;

/* ── Branch hints ─────────────────────────────────────────────────────── */
#if defined(__GNUC__) || defined(__clang__)
#  define JET_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define JET_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define JET_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#  define JET_LIKELY(x)   (x)
#  define JET_UNLIKELY(x) (x)
#  define JET_ALWAYS_INLINE inline
#endif

/* Per-operation statistics are OFF by default: 6 atomic RMWs per alloc/free
 * would dominate the hot path. Build with -DJET_STATS=1 to enable jet_get_stats
 * live counts. bytes_mapped / pages / large are always tracked (cold paths). */
#if defined(JET_STATS)
#  define JET_STAT_ADD(ctr, v) \
     atomic_fetch_add_explicit(&(ctr), (v), memory_order_relaxed)
#  define JET_STAT_SUB(ctr, v) \
     atomic_fetch_sub_explicit(&(ctr), (v), memory_order_relaxed)
#else
#  define JET_STAT_ADD(ctr, v) ((void)0)
#  define JET_STAT_SUB(ctr, v) ((void)0)
#endif

static JET_ALWAYS_INLINE jet_page* jet_page_of(const void* block) {
    return (jet_page*)((uintptr_t)block & JET_PAGE_MASK);
}

#endif /* JETALLOC_INTERNAL_H */
