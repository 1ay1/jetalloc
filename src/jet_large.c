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

/* Payload sits one page after the mapping base so the header has room and the
 * payload stays page-aligned. For align > JET_PAGE_SIZE we over-map and place
 * the header immediately before the aligned payload. */

void* jet_large_alloc(size_t size, size_t align) {
    if (align < JET_PAGE_SIZE) align = JET_PAGE_SIZE;

    /* Reserve header page + rounded payload. */
    size_t payload_rounded = (size + JET_PAGE_SIZE - 1) & ~((size_t)JET_PAGE_SIZE - 1);
    size_t total = JET_PAGE_SIZE + payload_rounded;

    void* base = jet_os_map_aligned(total, align);
    if (!base) return NULL;

    /* Payload begins one page in — that keeps it `align`-aligned because base
     * is `align`-aligned and align is a multiple of JET_PAGE_SIZE. */
    uint8_t* payload = (uint8_t*)base + JET_PAGE_SIZE;
    jet_large_hdr* h = (jet_large_hdr*)(payload - sizeof(jet_large_hdr));
    h->magic    = JET_LARGE_MAGIC;
    h->total    = total;
    h->payload  = size;
    h->align    = align;
    h->map_base = base;

    atomic_fetch_add_explicit(&jet_stat_large, 1, memory_order_relaxed);
    JET_STAT_ADD(jet_stat_live, size);
    return payload;
}

static inline jet_large_hdr* hdr_of(const void* ptr) {
    /* Large payloads are page-aligned; slab blocks never are (they sit after a
     * 64-byte page header). So a page-aligned pointer is a large-alloc
     * candidate — validate via the magic before trusting it. */
    if ((uintptr_t)ptr & (JET_PAGE_SIZE - 1)) return NULL;
    jet_large_hdr* h = (jet_large_hdr*)((const uint8_t*)ptr - sizeof(jet_large_hdr));
    if (h->magic != JET_LARGE_MAGIC) return NULL;
    return h;
}

int jet_large_free(void* ptr) {
    jet_large_hdr* h = hdr_of(ptr);
    if (!h) return 0;
    size_t payload = h->payload;
    void*  base    = h->map_base;
    size_t total   = h->total;
    (void)payload;  /* consumed only when JET_STATS is enabled */
    h->magic = 0;  /* poison so a double-free is detected as foreign */
    atomic_fetch_sub_explicit(&jet_stat_large, 1, memory_order_relaxed);
    JET_STAT_SUB(jet_stat_live, payload);
    jet_os_unmap(base, total);
    return 1;
}

size_t jet_large_usable(const void* ptr) {
    jet_large_hdr* h = hdr_of(ptr);
    if (!h) return 0;
    /* Usable = everything from payload start to end of the mapping. */
    return h->total - JET_PAGE_SIZE;
}

int jet_large_owns(const void* ptr) {
    return hdr_of(ptr) != NULL;
}
