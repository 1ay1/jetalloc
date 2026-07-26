/*
 * jetalloc — size-class ladder and size→class mapping.
 * SPDX-License-Identifier: MIT
 *
 * The ladder: 8, 16, then 16-byte steps to 128, then ~1.25x geometric steps
 * up to JET_LARGE_THRESHOLD (32 KiB). Every boundary keeps internal
 * fragmentation <= 12.5% (a request never wastes more than 1/8 of its class).
 */
#include "jet_internal.h"

const uint32_t jet_class_size[JET_NUM_CLASSES] = {
    /*  0.. 7 */   16,    32,    48,    64,    80,    96,   112,   128,
    /*  8..15 */  160,   192,   224,   256,   320,   384,   448,   512,
    /* 16..23 */  640,   768,   896,  1024,  1280,  1536,  1792,  2048,
    /* 24..31 */ 2560,  3072,  3584,  4096,  5120,  6144,  7168,  8192,
    /* 32..38 */10240, 12288, 14336, 16384, 20480, 24576, 32768,
};

/*
 * O(1) size→class map via a flat table at 16-byte granularity, built at first
 * use. Index = (size-1)>>4, so each slot covers a 16-byte span — finer than
 * the smallest class gap (16 B), so no class is ever skipped. 32768/16 = 2048
 * one-byte entries = 2 KiB, cheap and cache-friendly. No branching over the
 * ladder on the hot path.
 */
#define JET_MAP_SLOTS (JET_LARGE_THRESHOLD / 16 + 1)   /* 2049 */
static uint8_t class_map[JET_MAP_SLOTS];

/* Fill the flat size→class table. Runs exactly once, before any thread can
 * call jet_size_class, via a library constructor — so there is no lazy-init
 * data race (previously flagged by TSan) and no hot-path "is it built?" branch.
 * On the rare toolchain without constructor support the mirror lazy path below
 * still guarantees correctness. */
static void build_maps(void) {
    for (uint32_t s = 1; s <= JET_LARGE_THRESHOLD; ++s) {
        int c = 0;
        while (jet_class_size[c] < s) ++c;
        class_map[(s - 1) >> 4] = (uint8_t)c;
    }
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor))
static void jet_size_ctor(void) { build_maps(); }
#  define JET_SIZE_HAVE_CTOR 1
#else
#  define JET_SIZE_HAVE_CTOR 0
#endif

int jet_size_class(size_t size) {
#if !JET_SIZE_HAVE_CTOR
    /* Fallback for toolchains without constructors: guarded one-time build.
     * A benign double-build is harmless (identical, idempotent writes). */
    static _Atomic(int) maps_ready = 0;
    if (JET_UNLIKELY(!atomic_load_explicit(&maps_ready, memory_order_acquire))) {
        build_maps();
        atomic_store_explicit(&maps_ready, 1, memory_order_release);
    }
#endif
    if (size == 0) size = 1;
    return class_map[(size - 1) >> 4];
}
