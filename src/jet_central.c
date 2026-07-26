/*
 * jetalloc — central span pool.
 * SPDX-License-Identifier: MIT
 *
 * Carves 64 KiB slab pages out of 4 MiB OS spans and formats them for a size
 * class. Locked, but touched only when a thread cache runs dry, so a single
 * spinlock is cheap. Fully-empty pages are recycled here for any class.
 */
#include "jet_internal.h"
#include <string.h>

/* A minimal spinlock — the central pool is a cold path. */
typedef struct { _Atomic(int) v; } jet_spin;
static jet_spin central_lock = {0};

static inline void spin_lock(jet_spin* s) {
    for (;;) {
        if (!atomic_exchange_explicit(&s->v, 1, memory_order_acquire)) return;
        while (atomic_load_explicit(&s->v, memory_order_relaxed)) {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            __asm__ __volatile__("yield");
#endif
        }
    }
}
static inline void spin_unlock(jet_spin* s) {
    atomic_store_explicit(&s->v, 0, memory_order_release);
}

/* Free list of recycled empty pages, and the current bump cursor into the
 * most-recently mapped span. */
static jet_page* free_pages   = NULL;
static uint8_t*  span_cursor  = NULL;
static uint8_t*  span_end     = NULL;

/* ── Cache coloring ───────────────────────────────────────────────────────
 * Every page's block data would otherwise start at the SAME offset
 * (sizeof(jet_page) rounded up), so block N of every page maps to the SAME L1
 * cache set. Two hot objects at the same slot in different pages then conflict
 * and evict each other. We rotate each page's data start by a per-page COLOR
 * (a multiple of a cache line), spreading equivalent slots of consecutive pages
 * across different sets — the page-coloring trick from OS VM, applied in
 * user space. Cost: up to (COLORS-1)*LINE wasted bytes per page (<= ~1 KiB of
 * a 64 KiB page), reclaimed as capacity shrinks by at most one block. */
#define JET_COLOR_LINE   64u                  /* cache line                     */
#define JET_COLORS       16u                  /* distinct colors (1 KiB span)   */
static _Atomic(uint32_t) color_next = 0;      /* rotating page color            */
/* Pull one 64 KiB region: reuse a retired page, else bump within the current
 * span, else map a fresh span. Caller holds central_lock. Sets *mem_fresh to 1
 * only when the returned region is NEVER-WRITTEN OS memory (still zero from
 * mmap) — recycled pages from free_pages hold stale bytes, so *mem_fresh=0. */
static jet_page* raw_page(int* mem_fresh) {
    if (free_pages) {
        jet_page* pg = free_pages;
        free_pages = pg->next;
        *mem_fresh = 0;                 /* recycled — may hold stale data        */
        return pg;
    }
    if (span_cursor + JET_PAGE_SIZE > span_end) {
        uint8_t* span = (uint8_t*)jet_os_map_span(JET_SPAN_SIZE,
                                                 JET_PAGE_SIZE);
        if (!span) return NULL;
        span_cursor = span;
        span_end    = span + JET_SPAN_SIZE;
    }
    jet_page* pg = (jet_page*)span_cursor;
    span_cursor += JET_PAGE_SIZE;
    *mem_fresh = 1;                     /* fresh mmap — guaranteed zero          */
    return pg;
}

/* Format a raw region into a slab for `cls`-sized blocks. We do NOT thread a
 * free list through all blocks here — that would dirty every cache line in the
 * 64 KiB page up front. Instead we set a BUMP cursor over the data region:
 * the first `capacity` allocations are pure pointer arithmetic (bump += bs),
 * touching memory only as it's actually handed out. Recycled blocks later
 * flow through alloc_free/local_free (owner-local; cross-thread frees go to the
 * owner heap's inbox, never the page). */
static void format_page(jet_page* pg, jet_heap* h, int cls, int mem_fresh) {
    uint32_t bs = jet_class_size[cls];

    /* Pick this page's color (rotating). Relaxed atomic — format_page runs
     * unlocked and a racy duplicate color is harmless (just a weaker spread). */
    uint32_t color = atomic_fetch_add_explicit(
        &color_next, 1, memory_order_relaxed) % JET_COLORS;
    uintptr_t shift = (uintptr_t)color * JET_COLOR_LINE;

    uintptr_t base = (uintptr_t)pg;
    uintptr_t data = (base + sizeof(jet_page) + shift + (JET_MIN_ALIGN - 1))
                     & ~((uintptr_t)JET_MIN_ALIGN - 1);
    uintptr_t top  = base + JET_PAGE_SIZE;
    uint32_t cap   = (uint32_t)((top - data) / bs);

    pg->alloc_free  = NULL;                 /* no recycled blocks yet          */
    pg->bump        = (uint8_t*)data;       /* hand out fresh blocks from here */
    pg->bump_end    = (uint8_t*)(data + (size_t)cap * bs);
    pg->local_free  = NULL;
    pg->next        = NULL;
    pg->prev        = NULL;
    pg->owner       = h;
    pg->block_size  = bs;
    pg->capacity    = cap;
    pg->used        = 0;
    pg->cls         = (uint16_t)cls;
    pg->flags       = 0;
    pg->mem_fresh   = (uint8_t)mem_fresh;
    atomic_store_explicit(&pg->temp, 0, memory_order_relaxed);
    atomic_store_explicit(&pg->place_head, NULL, memory_order_relaxed);
    atomic_store_explicit(&pg->on_drain, 0, memory_order_relaxed);
    atomic_store_explicit(&pg->drain_next, NULL, memory_order_relaxed);
}

jet_page* jet_central_fresh_page(jet_heap* h, int cls) {
    int mem_fresh = 0;
    spin_lock(&central_lock);
    jet_page* pg = raw_page(&mem_fresh);
    spin_unlock(&central_lock);
    if (!pg) return NULL;
    format_page(pg, h, cls, mem_fresh);
    atomic_fetch_add_explicit(&jet_stat_pages, 1, memory_order_relaxed);
    return pg;
}

void jet_central_retire_page(jet_page* pg) {
    spin_lock(&central_lock);
    pg->next = free_pages;
    free_pages = pg;
    spin_unlock(&central_lock);
    atomic_fetch_sub_explicit(&jet_stat_pages, 1, memory_order_relaxed);
}
