/*
 * jetalloc — large (direct-mmap) allocations.
 * SPDX-License-Identifier: MIT
 *
 * Requests > JET_LARGE_THRESHOLD skip the slab machinery and get their own
 * mmap. We prefix each mapping with a header page carrying its size, so free /
 * usable_size / owns are O(1) with no global lookup — the header sits at
 * (payload - JET_PAGE_SIZE). Payloads are JET_PAGE_SIZE-aligned; the header's
 * magic lets jet_owns() cheaply distinguish our large blocks from foreign
 * pointers.
 */
#include "jet_internal.h"
#include <string.h>

#define JET_LARGE_MAGIC 0x6A65746C61726765ULL  /* "jetlarge" */

typedef struct jet_large_hdr {
    uint64_t magic;
    size_t   total;      /* header region + payload, as mapped               */
    size_t   payload;    /* bytes requested by the caller                    */
    size_t   align;      /* alignment honoured                               */
    void*    map_base;   /* actual mmap base (payload may be offset for align)*/
} jet_large_hdr;

/* ── Live-large-mapping registry (foreign/dangling page-aligned safety) ──────
 * hdr_of() reads the header that sits BEFORE the payload. For a page-aligned
 * pointer that jetalloc did NOT hand out — a foreign mmap buffer, or a large
 * block that was already freed (its mapping munmap'd) — that read touches
 * unmapped memory and SEGVs. But jet_owns()/jet_free()/jet_usable_size() must
 * be crash-proof on any page-aligned pointer: jet_owns is the interposer's
 * safety gate, and the whole point of the large-block magic was to "detect a
 * double-free as foreign" (impossible once the page is gone).
 *
 * So we track every LIVE large payload pointer. hdr_of() checks membership
 * FIRST and only dereferences a header it knows is mapped. Open-addressed hash
 * under a spinlock; large alloc/free are cold (they already do mmap/munmap),
 * so the lock and probe are free relative to the syscall. */
typedef struct { _Atomic(int) v; } jet_large_spin;
static jet_large_spin large_lock = {0};
static inline void large_lock_acquire(void) {
    while (atomic_exchange_explicit(&large_lock.v, 1, memory_order_acquire)) {
        while (atomic_load_explicit(&large_lock.v, memory_order_relaxed)) {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        }
    }
}
static inline void large_lock_release(void) {
    atomic_store_explicit(&large_lock.v, 0, memory_order_release);
}

/* Grow-only open-addressing set of live large payload pointers. Starts NULL;
 * first insert mmaps a table. On load-factor pressure it rehashes into a bigger
 * table. Tombstone-free because we compact on delete (linear-probe backshift). */
static void**  large_tab   = NULL;   /* table of payload pointers (NULL = empty) */
static size_t  large_cap   = 0;      /* power of two                             */
static size_t  large_count = 0;

static size_t large_hash(void* p, size_t cap) {
    uintptr_t x = (uintptr_t)p >> 16;   /* payloads are >=64KB aligned         */
    x *= 0x9E3779B97F4A7C15ULL;
    return (size_t)x & (cap - 1);
}

static int large_tab_insert(void** tab, size_t cap, void* p) {
    size_t i = large_hash(p, cap);
    for (size_t n = 0; n < cap; ++n) {
        if (tab[i] == NULL) { tab[i] = p; return 1; }
        if (tab[i] == p)    { return 1; }   /* already present                 */
        i = (i + 1) & (cap - 1);
    }
    return 0;   /* full (shouldn't happen: we keep load factor < 3/4)          */
}

static void large_register(void* payload) {
    large_lock_acquire();
    if (large_count * 4 >= large_cap * 3) {          /* grow at 75% load        */
        size_t ncap = large_cap ? large_cap * 2 : 64;
        void** ntab = (void**)jet_os_map(ncap * sizeof(void*));
        if (ntab) {
            for (size_t i = 0; i < large_cap; ++i)
                if (large_tab[i]) large_tab_insert(ntab, ncap, large_tab[i]);
            if (large_tab) jet_os_unmap(large_tab, large_cap * sizeof(void*));
            large_tab = ntab;
            large_cap = ncap;
        }
    }
    if (large_cap) large_tab_insert(large_tab, large_cap, payload);
    large_count++;
    large_lock_release();
}

static void large_unregister_locked(void* payload) {
    if (!large_cap) return;
    size_t i = large_hash(payload, large_cap);
    for (size_t n = 0; n < large_cap; ++n) {
        if (large_tab[i] == payload) {
            large_tab[i] = NULL;
            /* Linear-probe backshift: re-home any following cluster members so
             * lookups stay tombstone-free. */
            size_t j = (i + 1) & (large_cap - 1);
            while (large_tab[j]) {
                void* moved = large_tab[j];
                large_tab[j] = NULL;
                large_tab_insert(large_tab, large_cap, moved);
                j = (j + 1) & (large_cap - 1);
            }
            large_count--;
            return;
        }
        if (large_tab[i] == NULL) return;   /* not present                     */
        i = (i + 1) & (large_cap - 1);
    }
}

/* True iff `payload` is a currently-live large mapping we handed out. Safe to
 * call on any pointer — it only compares addresses, never dereferences them. */
static int large_is_live(void* payload) {
    if (!large_cap) return 0;
    size_t i = large_hash(payload, large_cap);
    for (size_t n = 0; n < large_cap; ++n) {
        if (large_tab[i] == payload) return 1;
        if (large_tab[i] == NULL)    return 0;
        i = (i + 1) & (large_cap - 1);
    }
    return 0;
}

/* Payload sits one page after the mapping base so the header has room and the
 * payload stays page-aligned. For align > JET_PAGE_SIZE we over-map and place
 * the header immediately before the aligned payload. */

void* jet_large_alloc(size_t size, size_t align) {
    if (align < JET_PAGE_SIZE) align = JET_PAGE_SIZE;

    /* Payload must be `align`-aligned AND have room for the header immediately
     * before it. The header is far smaller than a page, so one page (or, for
     * over-page alignments, one full `align` step) of headroom is plenty.
     *
     * jet_os_map_aligned returns an `align`-aligned base, so:
     *   - align == JET_PAGE_SIZE: the page after base is align-aligned and the
     *     header fits in that page  -> payload = base + JET_PAGE_SIZE.
     *   - align  > JET_PAGE_SIZE: base + JET_PAGE_SIZE is NOT align-aligned
     *     (base%align==0 but JET_PAGE_SIZE%align!=0). The first align-aligned
     *     address with header headroom is base + align  -> payload = base +
     *     align. This is the over-map the header comment always promised; it
     *     was previously unimplemented, so aligned_alloc / posix_memalign /
     *     over-aligned operator new silently returned only page-aligned memory
     *     for any alignment > 64 KiB (a C11/POSIX contract violation). */
    size_t headroom = (align > JET_PAGE_SIZE) ? align : JET_PAGE_SIZE;

    /* Reserve headroom + rounded payload. Guard every step against wrap: a huge
     * `size` (near SIZE_MAX) would otherwise round/round-add to a SMALL total,
     * mmap a tiny region, and hand back a pointer the caller believes is huge
     * -> heap overflow. Fail with NULL instead (malloc -> NULL, posix_memalign
     * -> ENOMEM), which is the correct out-of-memory answer. */
    if (size > SIZE_MAX - JET_PAGE_SIZE) return NULL;      /* rounding wraps    */
    size_t payload_rounded = (size + JET_PAGE_SIZE - 1) & ~((size_t)JET_PAGE_SIZE - 1);
    if (payload_rounded > SIZE_MAX - headroom) return NULL; /* total wraps      */
    size_t total = headroom + payload_rounded;

    void* base = jet_os_map_aligned(total, align);
    if (!base) return NULL;

    /* Payload begins `headroom` in — align-aligned because base is align-aligned
     * and headroom is a multiple of align (== align for over-page alignments,
     * == JET_PAGE_SIZE which divides align when align == JET_PAGE_SIZE). */
    uint8_t* payload = (uint8_t*)base + headroom;
    jet_large_hdr* h = (jet_large_hdr*)(payload - sizeof(jet_large_hdr));
    h->magic    = JET_LARGE_MAGIC;
    h->total    = total;
    h->payload  = size;
    h->align    = align;
    h->map_base = base;

    large_register(payload);
    atomic_fetch_add_explicit(&jet_stat_large, 1, memory_order_relaxed);
    JET_STAT_ADD(jet_stat_live, size);
    return payload;
}

static inline jet_large_hdr* hdr_of(const void* ptr) {
    /* Large payloads are page-aligned; slab blocks never are (they sit after a
     * 64-byte page header). So a page-aligned pointer is a large-alloc
     * candidate — but a FOREIGN or already-freed page-aligned pointer would
     * make the header read below fault. Prove the payload is a live large
     * mapping FIRST (address compare only, no deref), then the magic read is
     * guaranteed mapped. The magic is a cheap corruption backstop. */
    if ((uintptr_t)ptr & (JET_PAGE_SIZE - 1)) return NULL;
    large_lock_acquire();
    int live = large_is_live((void*)ptr);
    large_lock_release();
    if (!live) return NULL;
    jet_large_hdr* h = (jet_large_hdr*)((const uint8_t*)ptr - sizeof(jet_large_hdr));
    if (h->magic != JET_LARGE_MAGIC) return NULL;
    return h;
}

int jet_large_free(void* ptr) {
    if ((uintptr_t)ptr & (JET_PAGE_SIZE - 1)) return 0;   /* not page-aligned  */
    /* Take the lock across the liveness check AND the unregister so two
     * concurrent frees of the same pointer can't both reach munmap (only the
     * winner sees it live; the loser sees "not ours" and returns 0). */
    large_lock_acquire();
    if (!large_is_live(ptr)) { large_lock_release(); return 0; }
    jet_large_hdr* h = (jet_large_hdr*)((const uint8_t*)ptr - sizeof(jet_large_hdr));
    size_t payload = h->payload;
    void*  base    = h->map_base;
    size_t total   = h->total;
    h->magic = 0;              /* poison so a corrupted re-read is rejected     */
    large_unregister_locked(ptr);   /* remove before unmap                     */
    large_lock_release();
    (void)payload;  /* consumed only when JET_STATS is enabled */
    atomic_fetch_sub_explicit(&jet_stat_large, 1, memory_order_relaxed);
    JET_STAT_SUB(jet_stat_live, payload);
    jet_os_unmap(base, total);
    return 1;
}

size_t jet_large_usable(const void* ptr) {
    jet_large_hdr* h = hdr_of(ptr);
    if (!h) return 0;
    /* Usable = everything from the payload start to the end of the mapping.
     * The header offset (payload - map_base) is `headroom`, which is a full
     * page for align<=page but `align` for over-page alignments — so derive it
     * from the actual pointer rather than assuming one page. */
    size_t offset = (size_t)((const uint8_t*)ptr - (const uint8_t*)h->map_base);
    return h->total - offset;
}

int jet_large_owns(const void* ptr) {
    return hdr_of(ptr) != NULL;
}
