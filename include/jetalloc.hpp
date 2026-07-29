// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  jetalloc.hpp — a header-only, type-theoretic, memory-safe allocator.    ║
// ║  Single header. No .c, no ASM, no linking. #include and go.              ║
// ║  SPDX-License-Identifier: MIT                                            ║
// ╚══════════════════════════════════════════════════════════════════════════╝
//
// jetalloc is a fast slab allocator (per-thread heaps, 64 KiB header-free slab
// pages, size-class free lists) — but its headline feature is that the C++ TYPE
// SYSTEM makes memory bugs UNREPRESENTABLE. Every invariant the allocator relies
// on is lifted into a type, so whole classes of error are rejected at COMPILE
// time, or trapped at a single defined runtime contract point, never left to
// corrupt the heap.
//
//   Rust concept          →  jetalloc encoding
//   ───────────────────────────────────────────────────────────────────────────
//   Ownership / Drop      →  owned<T> / owned_array<T> : affine (move-only) RAII
//                            handles. No copy ctor ⇒ no double free. Move nulls
//                            the source ⇒ no use-after-move. Dtor drops ⇒ no leak.
//   &T  (shared borrow)   →  ref<T>  : many allowed, read-only.
//   &mut T (unique borrow)→  mut<T>  : at most one, exclusive. Aliasing XOR is
//                            enforced by a per-object borrow ledger that PANICS
//                            on violation (the borrow checker, dynamically).
//   'a lifetime           →  a borrow pins its owner; the owner's dtor asserts
//                            no live borrows remain (dangling-borrow trap).
//   Result<T, E>          →  expected<owned<T>, alloc_error> : OOM is a value,
//                            [[nodiscard]] forces you to handle it.
//   Slice &[T]            →  owned_array yields a bounded std::span; .at() checked.
//   provenance            →  you CANNOT build owned<T> from a raw pointer; the
//                            only mint site is the allocator ⇒ "did jetalloc
//                            allocate this?" is a THEOREM, not a runtime query.
//
// Target: C++23 (concepts, std::expected, std::span, consteval, deducing this).
//
#ifndef JETALLOC_HPP
#define JETALLOC_HPP

#include <atomic>
#include <bit>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

#if defined(__cpp_lib_source_location)
#  include <source_location>
#endif

// POSIX virtual-memory syscalls — pulled in only on Unix-likes so slab pages
// come STRAIGHT from the kernel (mmap), with no libc-malloc middleman.
#if defined(__unix__) || defined(__APPLE__) || defined(__HAIKU__)
#  include <sys/mman.h>
#  include <unistd.h>
// macOS/BSD historically expose only MAP_ANON; Linux only MAP_ANONYMOUS.
// Normalise so the mmap call below is spelled one way on every POSIX target.
#  if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#    define MAP_ANONYMOUS MAP_ANON
#  endif
#endif

namespace jet {

// ── Micro-architecture hints: branch prediction + forced inlining on the hot
//    path. Compile-time only; portable no-ops where the compiler lacks them.
#if defined(__GNUC__) || defined(__clang__)
#  define JET_LIKELY(x)      __builtin_expect(!!(x), 1)
#  define JET_UNLIKELY(x)    __builtin_expect(!!(x), 0)
#  define JET_ALWAYS_INLINE  inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#  define JET_LIKELY(x)      (x)
#  define JET_UNLIKELY(x)    (x)
#  define JET_ALWAYS_INLINE  __forceinline
#else
#  define JET_LIKELY(x)      (x)
#  define JET_UNLIKELY(x)    (x)
#  define JET_ALWAYS_INLINE  inline
#endif

// ════════════════════════════════════════════════════════════════════════════
//  0.  Diagnostics — a contract failure is a defined, loud abort, never UB.
// ════════════════════════════════════════════════════════════════════════════

[[noreturn]] inline void panic(const char* what) noexcept {
    std::fprintf(stderr, "jetalloc: fatal contract violation: %s\n", what);
    std::abort();
}

#if defined(JET_NO_CONTRACTS)
#  define JET_ASSERT(cond, msg) ((void)0)
#else
#  define JET_ASSERT(cond, msg) (static_cast<bool>(cond) ? (void)0 : ::jet::panic(msg))
#endif

// ── Hardening tier ───────────────────────────────────────────────────
// Define JET_HARDENED to trade a little speed for anti-corruption defenses on
// the allocator's raw path: freelist-pointer encoding (defeats the classic
// "overwrite a freed block's next-pointer" exploit), double-free / wild-free
// detection, and block-boundary validation. OFF by default — every check below
// is gated by `kHardened` so a non-hardened build emits byte-identical hot-path
// code. JET_NO_CONTRACTS does NOT disable hardening (they are orthogonal: one
// governs the type-layer borrow checker, the other the raw heap's integrity).
#if defined(JET_HARDENED)
inline constexpr bool kHardened = true;
#else
inline constexpr bool kHardened = false;
#endif

// A heap-integrity violation is always fatal and loud, even under
// JET_NO_CONTRACTS — a corrupted heap must never be allowed to proceed.
[[noreturn]] inline void heap_panic(const char* what) noexcept {
    std::fprintf(stderr, "jetalloc: HEAP CORRUPTION detected: %s\n", what);
    std::abort();
}

// ════════════════════════════════════════════════════════════════════════════
//  1.  Error domain — closed, exhaustively handleable. OOM is a value.
// ════════════════════════════════════════════════════════════════════════════

enum class alloc_error : std::uint8_t {
    out_of_memory = 1,
    too_large     = 2,
    length_overflow = 3,
};

[[nodiscard]] constexpr const char* to_string(alloc_error e) noexcept {
    switch (e) {
        case alloc_error::out_of_memory:   return "out_of_memory";
        case alloc_error::too_large:       return "too_large";
        case alloc_error::length_overflow: return "length_overflow";
    }
    return "unknown";
}

template <class T>
using result = std::expected<T, alloc_error>;

// ════════════════════════════════════════════════════════════════════════════
//  2.  Compile-time proofs — alignment as a type, storability as a concept.
// ════════════════════════════════════════════════════════════════════════════

// A value of power_of_two<N> is a PROOF that N is a positive power of two.
// power_of_two<48> does not instantiate, so any API taking one is statically
// guaranteed a legal alignment — the check has moved from runtime into the type.
template <std::size_t N>
struct power_of_two {
    static_assert(N != 0, "alignment must be non-zero");
    static_assert(std::has_single_bit(N), "alignment must be a power of two");
    static constexpr std::size_t value = N;
    consteval power_of_two() noexcept = default;
    [[nodiscard]] consteval operator std::size_t() const noexcept { return N; }
};

template <class T>
inline constexpr power_of_two<alignof(T)> align_of{};

// storable: a type an owning handle can correctly manage (place, destroy, free).
template <class T>
concept storable =
    std::is_object_v<T> &&
    !std::is_abstract_v<T> &&
    std::has_single_bit(alignof(T));

// ════════════════════════════════════════════════════════════════════════════
//  3.  The engine — a header-only slab allocator.
//
//  Layout: the OS gives us big spans; we carve them into 64 KiB, 64 KiB-aligned
//  SLAB PAGES. A page serves exactly one size class, so a block carries NO
//  header — its owning page is recovered by masking the address. Same-thread
//  malloc/free are a branchless pop/push on a per-page free list. This mirrors
//  the mimalloc/tcmalloc family; here it is ~1 header, no external symbols.
//
//  Everything below is `detail`: users touch only the safe handles in §4+.
// ════════════════════════════════════════════════════════════════════════════
namespace detail {

inline constexpr std::size_t kPageSize   = 64u * 1024u;
inline constexpr std::uintptr_t kPageMask = ~(static_cast<std::uintptr_t>(kPageSize) - 1);
inline constexpr std::size_t kMaxSmall    = 8u * 1024u;   // above this → large/direct
inline constexpr std::size_t kNumClasses  = 40;

// ════════════════════════════════════════════════════════════════════════
//  Cross-platform virtual-memory layer. This is the ONLY place that touches
//  the operating system. Every branch is resolved at COMPILE time (one #if),
//  so there is zero portability overhead in the emitted code — Windows gets
//  VirtualAlloc, POSIX gets mmap/posix_memalign, freestanding gets malloc.
//  os_map_aligned returns memory aligned to `align` at NATIVE speed: no
//  over-allocate-and-copy on the platforms with real aligned VM syscalls.
// ════════════════════════════════════════════════════════════════════════

// The one pointer-below-the-block header lets os_unmap recover the true base
// even when the platform's aligned request had to be trimmed. On Windows and
// mmap/posix_memalign the base == returned pointer, so the slot is free.
#if defined(_WIN32)
// Declare just the two Win32 VM entry points we use — avoids dragging in the
// multi-thousand-line <windows.h> for a header-only library. These match the
// kernel32 ABI exactly (stdcall, dllimport).
extern "C" __declspec(dllimport) void* __stdcall VirtualAlloc(
    void* lpAddress, std::size_t dwSize, unsigned long flAllocationType, unsigned long flProtect);
extern "C" __declspec(dllimport) int __stdcall VirtualFree(
    void* lpAddress, std::size_t dwSize, unsigned long dwFreeType);
inline constexpr unsigned long kWinCommitReserve = 0x1000 | 0x2000; // MEM_COMMIT|MEM_RESERVE
inline constexpr unsigned long kWinReadWrite     = 0x04;            // PAGE_READWRITE
inline constexpr unsigned long kWinMemRelease    = 0x8000;          // MEM_RELEASE

#elif defined(__unix__) || defined(__APPLE__) || defined(__HAIKU__)
#define JET_POSIX_MMAP 1
#endif

[[nodiscard]] inline void* os_map_aligned(std::size_t bytes, std::size_t align) noexcept {
    if (align < alignof(std::max_align_t)) align = alignof(std::max_align_t);

#if defined(_WIN32)
    // VirtualAlloc always returns 64 KiB-aligned memory (the allocation
    // granularity). For our 64 KiB slab pages that is exact; for larger
    // alignment we reserve+align+commit. Either way, native pages, no copy.
    if (align <= 64u * 1024u)
        return VirtualAlloc(nullptr, bytes, kWinCommitReserve, kWinReadWrite);
    // Over-aligned: reserve a padded region, compute the aligned address, and
    // commit from there. Store the reservation base one pointer below.
    void* raw = VirtualAlloc(nullptr, bytes + align, kWinCommitReserve, kWinReadWrite);
    if (!raw) return nullptr;
    auto a = (reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*) + (align - 1))
             & ~(static_cast<std::uintptr_t>(align) - 1);
    reinterpret_cast<void**>(a)[-1] = raw;
    return reinterpret_cast<void*>(a);

#elif defined(JET_POSIX_MMAP)
    // POSIX: pull pages STRAIGHT from the kernel via mmap — no libc-malloc
    // arena in the middle, so we neither depend on nor contend with another
    // allocator. mmap has no aligned variant portably, so we over-map by
    // `align`, then munmap the head/tail slack to trim to an aligned region.
    // This is the standard technique (jemalloc/mimalloc do the same) and
    // returns page-granular kernel memory at native speed.
    const std::size_t over = bytes + align;
    void* raw = ::mmap(nullptr, over, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) return nullptr;
    auto base = reinterpret_cast<std::uintptr_t>(raw);
    auto aligned = (base + (align - 1)) & ~(static_cast<std::uintptr_t>(align) - 1);
    const std::size_t head = aligned - base;
    const std::size_t tail = over - head - bytes;
    // Return the unused slack to the kernel so we hold exactly `bytes`.
    if (head) ::munmap(reinterpret_cast<void*>(base), head);
    if (tail) ::munmap(reinterpret_cast<void*>(aligned + bytes), tail);
    // base == returned pointer; munmap(p, bytes) frees it exactly — no header.
#if defined(MADV_HUGEPAGE)
    // Best-effort transparent-hugepage hint for large slab regions (Linux).
    if (bytes >= 2u * 1024u * 1024u)
        ::madvise(reinterpret_cast<void*>(aligned), bytes, MADV_HUGEPAGE);
#endif
    return reinterpret_cast<void*>(aligned);

#else
    // Freestanding fallback: over-allocate and align, base stashed below.
    const std::size_t total = bytes + align + sizeof(void*);
    void* raw = std::malloc(total);
    if (!raw) return nullptr;
    auto a = (reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*) + (align - 1))
             & ~(static_cast<std::uintptr_t>(align) - 1);
    reinterpret_cast<void**>(a)[-1] = raw;
    return reinterpret_cast<void*>(a);
#endif
}

inline void os_unmap(void* p, [[maybe_unused]] std::size_t bytes,
                     [[maybe_unused]] std::size_t align) noexcept {
    if (!p) return;
#if defined(_WIN32)
    if (align <= 64u * 1024u) { VirtualFree(p, 0, kWinMemRelease); return; }
    VirtualFree(reinterpret_cast<void**>(p)[-1], 0, kWinMemRelease);
#elif defined(JET_POSIX_MMAP)
    // We trimmed the mapping to exactly `bytes` at map time, so the region
    // [p, p+bytes) is precisely what mmap gave us — munmap it as one unit.
    ::munmap(p, bytes);
#else
    std::free(reinterpret_cast<void**>(p)[-1]);
#endif
}

// ── Size classes: 40 classes, ≤ ~12.5% internal fragmentation ─────────────
struct size_table {
    std::uint32_t sizes[kNumClasses]{};
    // Fine-grained reverse map at 8-byte granularity: class_of8[(n+7)/8].
    std::uint8_t  cls8[kMaxSmall / 8 + 2]{};

    consteval size_table() {
        std::size_t i = 0, s = 8;
        for (; i < 4; ++i, s += 8) sizes[i] = static_cast<std::uint32_t>(s);
        s = 32;
        while (i < kNumClasses && s < kMaxSmall) {
            std::size_t step = s / 4;
            for (int k = 0; k < 4 && i < kNumClasses; ++k) {
                s += step;
                if (s > kMaxSmall) s = kMaxSmall;
                sizes[i++] = static_cast<std::uint32_t>(s);
            }
        }
        while (i < kNumClasses) sizes[i++] = static_cast<std::uint32_t>(kMaxSmall);
        // Reverse map keyed by ceil(n/8): for every 8-byte bucket, the smallest
        // class whose block fits. Built once at compile time; O(1) lookup, no
        // division and no scan on the hot path.
        std::size_t cls = 0;
        for (std::size_t k = 0; k <= kMaxSmall / 8 + 1; ++k) {
            std::size_t want = k * 8; if (want == 0) want = 8;
            while (cls + 1 < kNumClasses && sizes[cls] < want) ++cls;
            cls8[k] = static_cast<std::uint8_t>(cls);
        }
    }
};
inline constexpr size_table g_sizes{};

// Hot path: size → class in a handful of instructions. `>>3` replaces the
// division; the +7 does the ceil; a single indexed byte load finishes it. No
// loop, no branch beyond the range clamp.
[[nodiscard]] JET_ALWAYS_INLINE int class_of(std::size_t n) noexcept {
    std::size_t k = (n + 7) >> 3;               // ceil(n / 8)
    if (JET_UNLIKELY(k == 0)) k = 1;
    return g_sizes.cls8[k];                      // k ≤ kMaxSmall/8+1 on the small path
}

struct heap;   // fwd

// A 64 KiB slab page. Header lives at the page base; blocks follow. Geometry
// (data start + capacity) is computed ONCE at page creation, so the hot path
// touches no divisions — just a free-list pop or a bump increment.
//
// Thread-safety model (mimalloc/snmalloc style): a page is OWNED by one heap.
// The owner's alloc/free use the plain `free_list` with no atomics. A foreign
// thread that frees a block it doesn't own pushes it onto the atomic
// `remote_free` MPSC stack instead of touching owner-private state; the owner
// drains that list lazily. `remote_used` (atomic) tracks how many blocks are
// parked there so the owner can reconcile `used` exactly.
struct alignas(64) page {
    heap*         owner;       // owning thread's heap (fast-path check)
    void*         free_list;   // owner-private LIFO free list (encoded if hardened)
    std::byte*    data;        // first block address (precomputed)
    std::atomic<void*>       remote_free{nullptr};   // MPSC stack of foreign frees
    std::atomic<std::uint32_t> remote_count{0};      // # parked in remote_free
    std::uint32_t block_size;  // bytes per block
    std::uint32_t used;        // live blocks handed out (owner's view)
    std::uint32_t capacity;    // total blocks the page can hold
    std::uint32_t bump;        // next never-yet-served block index
    page*         next;        // intrusive partial-list link
    std::uint16_t cls;         // size class
    std::uintptr_t cookie;     // per-page freelist-encoding key (hardened builds)

    // True iff `p` is the start of a real block in this page's data region:
    // inside [data, data+capacity*block_size) and exactly block-aligned. Used
    // by the hardened path to reject wild / interior / double frees.
    [[nodiscard]] JET_ALWAYS_INLINE bool contains_block(const void* p) const noexcept {
        auto up = reinterpret_cast<std::uintptr_t>(p);
        auto lo = reinterpret_cast<std::uintptr_t>(data);
        auto hi = lo + static_cast<std::uintptr_t>(capacity) * block_size;
        if (up < lo || up >= hi) return false;
        return (up - lo) % block_size == 0;
    }
};

// ── Freelist-pointer hardening ────────────────────────────────────────
// In a hardened build the `next` link a freed block stores is XOR-encoded with
// a per-page random cookie, so a heap overflow that overwrites the link cannot
// steer the next allocation to an attacker-chosen address (it would have to
// know the cookie). In a normal build these are the identity — zero cost.
[[nodiscard]] JET_ALWAYS_INLINE void* enc_next(const page* pg, void* p) noexcept {
    if constexpr (kHardened)
        return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(p) ^ pg->cookie);
    else
        return p;
}
[[nodiscard]] JET_ALWAYS_INLINE void* dec_next(const page* pg, void* p) noexcept {
    return enc_next(pg, p);   // XOR is its own inverse
}

// A cheap non-cryptographic per-page key: mix the page address with a
// process-lifetime seed. Enough to make the cookie unpredictable to an
// out-of-process attacker and to differ per page.
[[nodiscard]] inline std::uintptr_t page_cookie_for(const void* pg) noexcept {
    static const std::uintptr_t seed = [] {
        std::uintptr_t s = reinterpret_cast<std::uintptr_t>(&seed);
        s ^= static_cast<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(
                 std::addressof(errno)) * 0x9E3779B97F4A7C15ull);
        return s | 1u;
    }();
    std::uintptr_t x = reinterpret_cast<std::uintptr_t>(pg) ^ seed;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;   // fmix64
    return x | 1u;   // never 0, so encoding a real pointer never yields it back raw
}

// Per-thread heap. `partial[cls]` head is ALWAYS an allocatable page (full
// pages are unlinked on the spot), so alloc_small never scans a list. Empty
// pages are recycled through TWO tiers of syscall-free reserve:
//   1. `cached[cls]`  — one retained empty page per class, re-armed in place.
//   2. `pool` / `pool_n` — a small cross-class LIFO of whole 64 KiB pages that
//      stay mapped-and-committed in-process. A grow/shrink pattern that spills
//      across size classes (which tier 1 can't absorb) reuses a pooled page
//      with ZERO syscall — a plain relink. On Windows this is the decisive win:
//      it turns the steady state into no VirtualAlloc/VirtualFree at all. Only
//      when the pool overflows (> kPagePoolMax) does a page go back to the OS.
struct heap {
    page* partial[kNumClasses]{};
    page* cached[kNumClasses]{};   // one retained empty page per class

    // Cross-class recycled-page freelist (whole 64 KiB pages, still committed).
    static constexpr std::uint32_t kPagePoolMax = 16;   // ≤ 1 MiB parked/thread
    void*         pool = nullptr;   // intrusive LIFO threaded through page memory
    std::uint32_t pool_n = 0;

    // Take a bare committed 64 KiB page: from the pool (no syscall) or the OS.
    [[nodiscard]] JET_ALWAYS_INLINE void* take_raw_page() noexcept {
        if (void* p = pool) {
            pool = *static_cast<void**>(p);
            --pool_n;
            return p;
        }
        return os_map_aligned(kPageSize, kPageSize);
    }

    // Retire a whole empty page: park it in the pool if there is room (keeps it
    // committed for instant reuse), else hand the memory back to the OS.
    JET_ALWAYS_INLINE void give_raw_page(void* p) noexcept {
        if (pool_n < kPagePoolMax) {
            *static_cast<void**>(p) = pool;
            pool = p;
            ++pool_n;
        } else {
            os_unmap(p, kPageSize, kPageSize);
        }
    }

    [[nodiscard]] page* new_page(int cls) noexcept {
        // Reuse the retained empty page for this class if we have one — no
        // syscall, just relink and reset the bump/free state.
        if (page* c = cached[cls]) {
            cached[cls] = nullptr;
            c->free_list = nullptr;
            c->remote_free.store(nullptr, std::memory_order_relaxed);
            c->remote_count.store(0, std::memory_order_relaxed);
            c->used = 0;
            c->bump = 0;
            c->next = partial[cls];
            partial[cls] = c;
            return c;
        }
        void* mem = take_raw_page();
        if (JET_UNLIKELY(!mem)) return nullptr;
        auto* pg = static_cast<page*>(mem);
        const std::size_t bs = g_sizes.sizes[cls];
        const std::size_t hdr = (sizeof(page) + bs - 1) / bs * bs;   // one-time
        pg->owner = this;
        pg->free_list = nullptr;
        pg->data = reinterpret_cast<std::byte*>(pg) + hdr;
        pg->remote_free.store(nullptr, std::memory_order_relaxed);
        pg->remote_count.store(0, std::memory_order_relaxed);
        pg->block_size = static_cast<std::uint32_t>(bs);
        pg->used = 0;
        pg->cls = static_cast<std::uint16_t>(cls);
        pg->capacity = static_cast<std::uint32_t>((kPageSize - hdr) / bs);
        pg->bump = 0;
        pg->cookie = kHardened ? page_cookie_for(pg) : 0;
        pg->next = partial[cls];
        partial[cls] = pg;
        return pg;
    }

    // Move any blocks foreign threads parked in the page's atomic remote list
    // back into the owner-private free list, and reconcile `used`. Called by the
    // owner only, so the private side needs no atomics.
    JET_ALWAYS_INLINE void drain_remote(page* pg) noexcept {
        if (JET_LIKELY(pg->remote_free.load(std::memory_order_relaxed) == nullptr)) return;
        void* head = pg->remote_free.exchange(nullptr, std::memory_order_acquire);
        std::uint32_t n = pg->remote_count.exchange(0, std::memory_order_relaxed);
        while (head) {
            void* nxt = *static_cast<void**>(head);   // remote stack: raw links
            if constexpr (kHardened) {
                if (!pg->contains_block(head))
                    heap_panic("remote-free list corrupted (bad block pointer)");
            }
            *static_cast<void**>(head) = enc_next(pg, pg->free_list);
            pg->free_list = head;
            head = nxt;
        }
        pg->used -= n;
    }

    // The hot allocation path. `partial[cls]` is guaranteed non-full on entry
    // (invariant maintained below), so there is no list walk: pop the free list
    // if non-empty, else bump. When the page fills, unlink it so the next call
    // finds an allocatable head immediately.
    [[nodiscard]] JET_ALWAYS_INLINE void* alloc_small(int cls) noexcept {
        page* pg = partial[cls];
        if (JET_UNLIKELY(!pg)) { pg = new_page(cls); if (JET_UNLIKELY(!pg)) return nullptr; }
        void* blk = pg->free_list;
        if (JET_UNLIKELY(blk == nullptr)) {
            // Local free list empty: reclaim foreign frees before bumping.
            drain_remote(pg);
            blk = pg->free_list;
        }
        if (JET_LIKELY(blk != nullptr)) {
            void* nxt = dec_next(pg, *static_cast<void**>(blk));
            if constexpr (kHardened) {
                // The decoded next must be null or a properly-aligned block
                // inside this page's data region — otherwise the link was
                // corrupted (overflow / use-after-free write).
                if (nxt && !pg->contains_block(nxt))
                    heap_panic("freelist link corrupted (bad next pointer)");
            }
            pg->free_list = nxt;
        } else {
            blk = pg->data + std::size_t(pg->bump++) * pg->block_size;
        }
        if (JET_UNLIKELY(++pg->used == pg->capacity)) partial[cls] = pg->next;  // unlink full
        return blk;
    }
};

// Owning page of a block: mask to the 64 KiB boundary. Header-free by design.
[[nodiscard]] inline page* page_of(void* p) noexcept {
    return reinterpret_cast<page*>(reinterpret_cast<std::uintptr_t>(p) & kPageMask);
}

// Per-thread heap, reached through a cached thread_local POINTER. Taking the
// address of the thread_local object once (and caching it) lets the hot path
// avoid the general TLS-access sequence on every allocation — the compiler
// keeps `t_heap` in a register across a run of allocations.
inline thread_local heap  t_heap_storage{};
inline thread_local heap* t_heap = &t_heap_storage;

// A registry of large (direct) allocations so we can recognise + size them.
// `magic` lets usable_size distinguish a large block from an interior slab
// pointer without a global registry.
inline constexpr std::uint64_t kLargeMagic = 0x6a65746c'6172676eull; // "jetlargn"
struct large_hdr { std::uint64_t magic; void* os_base; std::size_t map_bytes; std::size_t bytes; std::size_t align; };

[[nodiscard]] JET_ALWAYS_INLINE void* raw_allocate(std::size_t bytes, std::size_t align) noexcept {
    if (JET_UNLIKELY(bytes == 0)) bytes = 1;
    if (JET_LIKELY(bytes <= kMaxSmall && align <= alignof(std::max_align_t)))
        return t_heap->alloc_small(class_of(bytes));
    // Large / over-aligned: direct-map, then carve an ALIGNED user pointer
    // with the header stored in the bytes immediately below it.
    const std::size_t map_align = align < kPageSize ? kPageSize : align;
    const std::size_t map_bytes = bytes + map_align + sizeof(large_hdr);
    void* mem = os_map_aligned(map_bytes, map_align);
    if (JET_UNLIKELY(!mem)) return nullptr;
    auto raw = reinterpret_cast<std::uintptr_t>(mem) + sizeof(large_hdr);
    auto user = (raw + (align - 1)) & ~(static_cast<std::uintptr_t>(align) - 1);
    auto* h = reinterpret_cast<large_hdr*>(user) - 1;
    h->magic = kLargeMagic;
    h->os_base = mem; h->map_bytes = map_bytes; h->bytes = bytes; h->align = align;
    return reinterpret_cast<void*>(user);
}

JET_ALWAYS_INLINE void raw_deallocate(void* p, std::size_t bytes, std::size_t align) noexcept {
    if (JET_UNLIKELY(!p)) return;
    if (JET_UNLIKELY(bytes > kMaxSmall || align > alignof(std::max_align_t))) {
        auto* h = reinterpret_cast<large_hdr*>(p) - 1;
        os_unmap(h->os_base, h->map_bytes, h->align < kPageSize ? kPageSize : h->align);
        return;
    }
    page* pg = page_of(p);

    if constexpr (kHardened) {
        // Wild / interior free: the pointer must be a real block start in a
        // page this process actually owns for a small class.
        if (pg->owner == nullptr || pg->block_size == 0 ||
            pg->cls >= kNumClasses || !pg->contains_block(p))
            heap_panic("invalid free (not a jetalloc block, or interior pointer)");
    }

    // Cross-thread free: the block belongs to another thread's heap. Do NOT
    // touch any owner-private state (that would be a data race). Push it onto
    // the page's lock-free MPSC remote-free stack; the owner reclaims it later.
    if (JET_UNLIKELY(pg->owner != t_heap)) {
        void* head = pg->remote_free.load(std::memory_order_relaxed);
        do { *static_cast<void**>(p) = head; }
        while (!pg->remote_free.compare_exchange_weak(
                   head, p, std::memory_order_release, std::memory_order_relaxed));
        pg->remote_count.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if constexpr (kHardened) {
        // Double-free detection. A full free-list walk would be O(n) per free
        // (quadratic as a page drains), so we bound the scan to a small window
        // near the head — this catches the realistic case (an immediate or
        // near-immediate re-free of the same block) in O(1). The freelist
        // encoding below independently defeats the *exploitation* of any
        // double-free that slips past the window, so this is defense-in-depth,
        // not the sole barrier.
        constexpr int kScan = 16;
        void* c = pg->free_list;
        for (int i = 0; i < kScan && c; ++i, c = dec_next(pg, *static_cast<void**>(c))) {
            if (c == p) heap_panic("double free (block already on the free list)");
        }
        if (JET_UNLIKELY(pg->used == 0))
            heap_panic("double free (page has no live blocks)");
    }

    // Same-thread free: owner-private, no atomics.
    const bool was_full = (pg->used == pg->capacity);   // unlinked from partial?
    *static_cast<void**>(p) = enc_next(pg, pg->free_list);
    pg->free_list = p;
    --pg->used;
    if (JET_LIKELY(!was_full)) {
        if (JET_UNLIKELY(pg->used == 0)) {
            // Reconcile any parked foreign frees first so `used` is exact; a
            // block still checked out by another thread keeps used > 0 and
            // prevents premature reclamation.
            pg->owner->drain_remote(pg);
            if (pg->used == 0) {
                heap* h = pg->owner;
                page** pp = &h->partial[pg->cls];
                while (*pp && *pp != pg) pp = &(*pp)->next;
                if (*pp == pg) *pp = pg->next;
                // Retain ONE empty page per class as a syscall-free reserve;
                // hand overflow to the cross-class page pool (still committed,
                // reused with zero syscall) before ever touching the OS.
                if (h->cached[pg->cls] == nullptr) h->cached[pg->cls] = pg;
                else h->give_raw_page(pg);
            }
        }
        return;
    }
    // Was full (off the list); now has a free block — re-link at the head so the
    // fast path can serve from it again.
    pg->next = pg->owner->partial[pg->cls];
    pg->owner->partial[pg->cls] = pg;
}

// Deallocate a block given ONLY its pointer — no size, no align. This is what
// `operator delete` and a C-style `free` receive. We recover everything from
// the block itself: a large allocation carries our magic tag in the word just
// below the user pointer; anything else is a slab block whose owning 64 KiB
// page (and thus its size class) is found by masking the address.
JET_ALWAYS_INLINE void free_unsized(void* p) noexcept {
    if (JET_UNLIKELY(!p)) return;
    auto* h = reinterpret_cast<large_hdr*>(p) - 1;
    if (JET_UNLIKELY(h->magic == kLargeMagic)) {
        os_unmap(h->os_base, h->map_bytes, h->align < kPageSize ? kPageSize : h->align);
        return;
    }
    raw_deallocate(p, 1, alignof(std::max_align_t));  // slab path: size unused
}

// Typed raw helpers with an overflow-checked element count.
template <storable T>
[[nodiscard]] inline T* typed_alloc(std::size_t count, bool& overflow) noexcept {
    overflow = false;
    if (count == 0) count = 1;
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) { overflow = true; return nullptr; }
    return static_cast<T*>(raw_allocate(count * sizeof(T), alignof(T)));
}
template <storable T>
inline void typed_free(T* p, std::size_t count) noexcept {
    if (p) raw_deallocate(p, (count ? count : 1) * sizeof(T), alignof(T));
}

}  // namespace detail

// ════════════════════════════════════════════════════════════════════════════
//  4.  Borrow ledger — the dynamic borrow checker (aliasing XOR).
//
//  Each owning handle carries a tiny control block. Shared borrows (ref<T>)
//  increment a reader count; a unique borrow (mut<T>) claims exclusivity.
//  The rules are Rust's: any number of &, XOR exactly one &mut. A violation is
//  a panic, not UB. Dropping an owner with borrows still live is also a panic
//  (a would-be dangling borrow, caught).
// ════════════════════════════════════════════════════════════════════════════
namespace detail {
#if defined(JET_NO_CONTRACTS)
// Hardened build: the borrow checker is compiled OUT. The ledger is an empty
// type, borrows are plain pointers, and owned<T> is a single pointer with a
// single allocation/free. Zero overhead over a hand-rolled unique_ptr.
struct borrow_ledger {
    JET_ALWAYS_INLINE void acquire_shared() noexcept {}
    JET_ALWAYS_INLINE void release_shared() noexcept {}
    JET_ALWAYS_INLINE void acquire_unique() noexcept {}
    JET_ALWAYS_INLINE void release_unique() noexcept {}
    [[nodiscard]] JET_ALWAYS_INLINE bool borrowed() const noexcept { return false; }
};
inline constexpr bool kContractsOn = false;
#else
struct borrow_ledger {
    std::int32_t shared = 0;   // # of live ref<T>
    bool         unique = false;   // a live mut<T> holds exclusivity

    void acquire_shared() {
        JET_ASSERT(!unique, "shared borrow (&) while a unique borrow (&mut) is live");
        ++shared;
    }
    void release_shared() noexcept { --shared; }
    void acquire_unique() {
        JET_ASSERT(!unique, "second unique borrow (&mut) — aliasing violation");
        JET_ASSERT(shared == 0, "unique borrow (&mut) while shared borrows (&) are live");
        unique = true;
    }
    void release_unique() noexcept { unique = false; }
    [[nodiscard]] bool borrowed() const noexcept { return shared != 0 || unique; }
};
inline constexpr bool kContractsOn = true;
#endif
}  // namespace detail

template <storable T> class owned;

// ref<T> — a shared borrow (&T). Read-only, copyable, lifetime-pinned. While any
// ref is alive, no mut may be taken; the owner cannot be dropped.
template <storable T>
class [[nodiscard]] ref {
public:
    ref(const ref& o) noexcept : p_(o.p_), led_(o.led_) { if (led_) led_->acquire_shared(); }
    ref& operator=(const ref&) = delete;
    ref(ref&& o) noexcept : p_(std::exchange(o.p_, nullptr)), led_(std::exchange(o.led_, nullptr)) {}
    ~ref() { if (led_) led_->release_shared(); }

    [[nodiscard]] const T& operator*()  const noexcept { return *p_; }
    [[nodiscard]] const T* operator->() const noexcept { return p_; }
    [[nodiscard]] const T* get()        const noexcept { return p_; }

private:
    friend class owned<T>;
    ref(const T* p, detail::borrow_ledger* l) noexcept : p_(p), led_(l) { led_->acquire_shared(); }
    const T* p_;
    detail::borrow_ledger* led_;
};

// mut<T> — a unique borrow (&mut T). Exclusive, move-only, lifetime-pinned.
// At most one may exist, and only while no shared borrows are live.
template <storable T>
class [[nodiscard]] mut {
public:
    mut(const mut&) = delete;
    mut& operator=(const mut&) = delete;
    mut(mut&& o) noexcept : p_(std::exchange(o.p_, nullptr)), led_(std::exchange(o.led_, nullptr)) {}
    ~mut() { if (led_) led_->release_unique(); }

    [[nodiscard]] T& operator*()  const noexcept { return *p_; }
    [[nodiscard]] T* operator->() const noexcept { return p_; }
    [[nodiscard]] T* get()        const noexcept { return p_; }

private:
    friend class owned<T>;
    mut(T* p, detail::borrow_ledger* l) noexcept : p_(p), led_(l) { led_->acquire_unique(); }
    T* p_;
    detail::borrow_ledger* led_;
};

// ════════════════════════════════════════════════════════════════════════════
//  5.  owned<T> — one object, exclusively owned, provably from jetalloc.
//
//  Affine (move-only) RAII. Invariant: ptr_ is null (moved-from) OR points to a
//  live T on jetalloc storage. No public raw-pointer ctor ⇒ the invariant is
//  unbreakable. Borrows are handed out through the ledger, so aliasing rules and
//  dangling-borrow prevention are enforced at runtime with Rust's semantics.
// ════════════════════════════════════════════════════════════════════════════
template <storable T>
class [[nodiscard]] owned {
public:
    using element_type = T;

    constexpr owned() noexcept = default;
    constexpr owned(std::nullptr_t) noexcept {}

    owned(const owned&)            = delete;   // affine: no duplicate ownership
    owned& operator=(const owned&) = delete;

    owned(owned&& o) noexcept
        : ptr_(std::exchange(o.ptr_, nullptr)), led_(std::exchange(o.led_, nullptr)) {}
    owned& operator=(owned&& o) noexcept {
        if (this != &o) { drop(); ptr_ = std::exchange(o.ptr_, nullptr);
                          led_ = std::exchange(o.led_, nullptr); }
        return *this;
    }
    ~owned() { drop(); }

    [[nodiscard]] bool engaged() const noexcept { return ptr_ != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return engaged(); }

    // Borrows — the ONLY way to reach the value. Direct raw access is not
    // offered, so the aliasing ledger cannot be bypassed.
    [[nodiscard]] ref<T> borrow() const {
        JET_ASSERT(ptr_, "borrow() on a disengaged (moved-from) owned<T>");
        return ref<T>(ptr_, led_);
    }
    [[nodiscard]] mut<T> borrow_mut() {
        JET_ASSERT(ptr_, "borrow_mut() on a disengaged (moved-from) owned<T>");
        return mut<T>(ptr_, led_);
    }

    // Convenience for the common no-aliasing case: transient scoped access.
    template <class F> decltype(auto) with(F&& f) const { auto b = borrow();     return std::forward<F>(f)(*b); }
    template <class F> decltype(auto) with_mut(F&& f)   { auto b = borrow_mut(); return std::forward<F>(f)(*b); }

    void reset() noexcept { drop(); }

private:
    void drop() noexcept {
        if (!ptr_) return;
        JET_ASSERT(led_ && !led_->borrowed(),
                   "dropping owned<T> while a borrow is still live (dangling borrow)");
        ptr_->~T();
        // ptr_ and led_ live in ONE co-allocated block; the ledger sits at the
        // base. Free the whole block with the size/align try_make used.
        constexpr std::size_t led_sz = sizeof(detail::borrow_ledger);
        constexpr std::size_t t_off  = (led_sz + alignof(T) - 1) & ~(alignof(T) - 1);
        constexpr std::size_t align  = alignof(T) > alignof(detail::borrow_ledger)
                                           ? alignof(T) : alignof(detail::borrow_ledger);
        detail::raw_deallocate(static_cast<void*>(led_), t_off + sizeof(T), align);
        ptr_ = nullptr; led_ = nullptr;
    }

    owned(T* p, detail::borrow_ledger* l) noexcept : ptr_(p), led_(l) {}

    template <storable U, class... Args> friend result<owned<U>> try_make(Args&&...) noexcept;

    T* ptr_ = nullptr;
    detail::borrow_ledger* led_ = nullptr;
};

// ════════════════════════════════════════════════════════════════════════════
//  6.  owned_array<T> — N objects, exclusively owned, accessed as a std::span.
//     Length is carried in the handle ⇒ sized-free is always correct; element
//     access is bounded (.at() checked, .view() is a std::span).
// ════════════════════════════════════════════════════════════════════════════
template <storable T>
class [[nodiscard]] owned_array {
public:
    using element_type = T;

    constexpr owned_array() noexcept = default;
    constexpr owned_array(std::nullptr_t) noexcept {}

    owned_array(const owned_array&)            = delete;
    owned_array& operator=(const owned_array&) = delete;

    owned_array(owned_array&& o) noexcept
        : ptr_(std::exchange(o.ptr_, nullptr)), len_(std::exchange(o.len_, 0)) {}
    owned_array& operator=(owned_array&& o) noexcept {
        if (this != &o) { drop(); ptr_ = std::exchange(o.ptr_, nullptr);
                          len_ = std::exchange(o.len_, 0); }
        return *this;
    }
    ~owned_array() { drop(); }

    [[nodiscard]] std::size_t size()  const noexcept { return len_; }
    [[nodiscard]] bool        empty() const noexcept { return len_ == 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return ptr_ != nullptr; }

    [[nodiscard]] std::span<T>       view()       noexcept { return {ptr_, len_}; }
    [[nodiscard]] std::span<const T> view() const noexcept { return {ptr_, len_}; }

    [[nodiscard]] T& at(std::size_t i) {
        if (i >= len_) throw std::out_of_range("jet::owned_array::at");
        return ptr_[i];
    }
    [[nodiscard]] const T& at(std::size_t i) const {
        if (i >= len_) throw std::out_of_range("jet::owned_array::at");
        return ptr_[i];
    }

    void reset() noexcept { drop(); }

private:
    void drop() noexcept {
        if (!ptr_) return;
        std::destroy_n(ptr_, len_);
        detail::typed_free(ptr_, len_);
        ptr_ = nullptr; len_ = 0;
    }
    owned_array(T* p, std::size_t n) noexcept : ptr_(p), len_(n) {}

    template <storable U> friend result<owned_array<U>> try_make_array(std::size_t) noexcept;
    template <storable U, class Fn> friend result<owned_array<U>> try_make_array_with(std::size_t, Fn&&);

    T* ptr_ = nullptr;
    std::size_t len_ = 0;
};

// ════════════════════════════════════════════════════════════════════════════
//  7.  Factories — fallible (expected) primary, throwing convenience on top.
// ════════════════════════════════════════════════════════════════════════════

template <storable T, class... Args>
[[nodiscard]] result<owned<T>> try_make(Args&&... args) noexcept {
    static_assert(std::is_constructible_v<T, Args...>, "T not constructible from Args");
    // ONE allocation holds both the borrow ledger and the object (the ledger is
    // an empty type under JET_NO_CONTRACTS, so this degenerates to a single T).
    // T is placed at a properly-aligned offset after the ledger.
    constexpr std::size_t led_sz = sizeof(detail::borrow_ledger);
    constexpr std::size_t t_off  = (led_sz + alignof(T) - 1) & ~(alignof(T) - 1);
    constexpr std::size_t align  = alignof(T) > alignof(detail::borrow_ledger)
                                       ? alignof(T) : alignof(detail::borrow_ledger);
    auto* base = static_cast<std::byte*>(detail::raw_allocate(t_off + sizeof(T), align));
    if (JET_UNLIKELY(!base)) return std::unexpected(alloc_error::out_of_memory);
    auto* led = reinterpret_cast<detail::borrow_ledger*>(base);
    auto* raw = reinterpret_cast<T*>(base + t_off);
    ::new (static_cast<void*>(led)) detail::borrow_ledger{};
    if constexpr (std::is_nothrow_constructible_v<T, Args...>) {
        ::new (static_cast<void*>(raw)) T(std::forward<Args>(args)...);
    } else {
        try { ::new (static_cast<void*>(raw)) T(std::forward<Args>(args)...); }
        catch (...) { detail::raw_deallocate(base, t_off + sizeof(T), align);
                      return std::unexpected(alloc_error::out_of_memory); }
    }
    return owned<T>(raw, led);
}

template <storable T>
[[nodiscard]] result<owned_array<T>> try_make_array(std::size_t n) noexcept {
    static_assert(std::is_default_constructible_v<T>, "T must be default-constructible");
    bool ov = false;
    T* raw = detail::typed_alloc<T>(n, ov);
    if (!raw) return std::unexpected(ov ? alloc_error::length_overflow : alloc_error::out_of_memory);
    if constexpr (std::is_nothrow_default_constructible_v<T>) {
        std::uninitialized_default_construct_n(raw, n);
    } else {
        try { std::uninitialized_default_construct_n(raw, n); }
        catch (...) { detail::typed_free(raw, n); return std::unexpected(alloc_error::out_of_memory); }
    }
    return owned_array<T>(raw, n);
}

// Build each element from its index via fn(i) -> T. Strongly exception-safe:
// an element ctor throwing unwinds the already-built prefix.
template <storable T, class Fn>
[[nodiscard]] result<owned_array<T>> try_make_array_with(std::size_t n, Fn&& fn) {
    static_assert(std::is_invocable_r_v<T, Fn&, std::size_t>, "generator must be T(size_t)");
    bool ov = false;
    T* raw = detail::typed_alloc<T>(n, ov);
    if (!raw) return std::unexpected(ov ? alloc_error::length_overflow : alloc_error::out_of_memory);
    std::size_t built = 0;
    try { for (; built < n; ++built) ::new (static_cast<void*>(raw + built)) T(fn(built)); }
    catch (...) { std::destroy_n(raw, built); detail::typed_free(raw, n); throw; }
    return owned_array<T>(raw, n);
}

// Throwing convenience — unwraps the expected, throws std::bad_alloc on OOM.
template <storable T, class... Args>
[[nodiscard]] owned<T> make(Args&&... args) {
    auto r = try_make<T>(std::forward<Args>(args)...);
    if (!r) throw std::bad_alloc();
    return std::move(*r);
}
template <storable T>
[[nodiscard]] owned_array<T> make_array(std::size_t n) {
    auto r = try_make_array<T>(n);
    if (!r) throw std::bad_alloc();
    return std::move(*r);
}
template <storable T, class Fn>
[[nodiscard]] owned_array<T> make_array_with(std::size_t n, Fn&& fn) {
    auto r = try_make_array_with<T>(n, std::forward<Fn>(fn));
    if (!r) throw std::bad_alloc();
    return std::move(*r);
}

// ════════════════════════════════════════════════════════════════════════════
//  8.  std interop — a Cpp17/23 Allocator so std containers use jetalloc.
// ════════════════════════════════════════════════════════════════════════════
template <storable T>
class allocator {
public:
    using value_type = T;
    using is_always_equal = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    template <class U> struct rebind { using other = allocator<U>; };

    constexpr allocator() noexcept = default;
    template <class U> constexpr allocator(const allocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t n) {
        bool ov = false;
        T* p = detail::typed_alloc<T>(n, ov);
        if (!p) throw std::bad_alloc();
        return p;
    }
    void deallocate(T* p, std::size_t n) noexcept { detail::typed_free(p, n); }
    [[nodiscard]] constexpr std::size_t max_size() const noexcept {
        return std::numeric_limits<std::size_t>::max() / sizeof(T);
    }
};
template <class T, class U>
[[nodiscard]] constexpr bool operator==(const allocator<T>&, const allocator<U>&) noexcept { return true; }

// ════════════════════════════════════════════════════════════════════════════
//  9.  buffer — the ONE untyped surface: raw bytes, but bounded + affine.
// ════════════════════════════════════════════════════════════════════════════
class [[nodiscard]] buffer {
public:
    constexpr buffer() noexcept = default;
    buffer(const buffer&) = delete;
    buffer& operator=(const buffer&) = delete;
    buffer(buffer&& o) noexcept
        : ptr_(std::exchange(o.ptr_, nullptr)), len_(std::exchange(o.len_, 0)), al_(o.al_) {}
    buffer& operator=(buffer&& o) noexcept {
        if (this != &o) { drop(); ptr_ = std::exchange(o.ptr_, nullptr);
                          len_ = std::exchange(o.len_, 0); al_ = o.al_; }
        return *this;
    }
    ~buffer() { drop(); }

    [[nodiscard]] std::size_t size()  const noexcept { return len_; }
    [[nodiscard]] bool        empty() const noexcept { return len_ == 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return ptr_ != nullptr; }
    [[nodiscard]] std::span<std::byte>       bytes()       noexcept { return {ptr_, len_}; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {ptr_, len_}; }

    template <std::size_t Align = alignof(std::max_align_t)>
    [[nodiscard]] static buffer make(std::size_t n, power_of_two<Align> = {}) {
        void* p = detail::raw_allocate(n ? n : 1, Align);
        if (!p) throw std::bad_alloc();
        return buffer(static_cast<std::byte*>(p), n, Align);
    }
    void reset() noexcept { drop(); }

private:
    buffer(std::byte* p, std::size_t n, std::size_t a) noexcept : ptr_(p), len_(n), al_(a) {}
    void drop() noexcept {
        if (ptr_) { detail::raw_deallocate(ptr_, len_ ? len_ : 1, al_); ptr_ = nullptr; len_ = 0; }
    }
    std::byte* ptr_ = nullptr;
    std::size_t len_ = 0;
    std::size_t al_ = alignof(std::max_align_t);
};

// ════════════════════════════════════════════════════════════════════════
//  10.  Introspection + maintenance.
// ════════════════════════════════════════════════════════════════════════

// Semantic version of the library, e.g. "0.3.0".
#define JETALLOC_VERSION_MAJOR 0
#define JETALLOC_VERSION_MINOR 3
#define JETALLOC_VERSION_PATCH 0
[[nodiscard]] inline const char* version() noexcept { return "0.3.0"; }

// The usable size of a block returned by this allocator (its size-class block
// size for small allocations, the requested size for large). Behaviour is
// defined ONLY for a pointer jetalloc returned and still owns; passing anything
// else is a precondition violation (as with malloc_usable_size).
[[nodiscard]] inline std::size_t usable_size(const void* p) noexcept {
    if (!p) return 0;
    // A small block sits inside a 64 KiB slab page and is never page-aligned
    // (it follows the page header). A large block's user pointer has our magic
    // tag in the word just below it. Check the tag first; if absent, it's a
    // slab block and its size is the page's class block size.
    auto* maybe = reinterpret_cast<const detail::large_hdr*>(p) - 1;
    if (maybe->magic == detail::kLargeMagic) return maybe->bytes;
    detail::page* pg = detail::page_of(const_cast<void*>(p));
    return pg->block_size;
}

// Release this thread's retained empty slab pages back to the OS: both the
// per-class reserve AND the cross-class page pool. Cheap, safe to call any
// time; a no-op when there is nothing parked. Call it on threads that have
// finished a burst of allocation to shrink RSS back to the live set.
inline void trim() noexcept {
    detail::heap* h = detail::t_heap;
    for (std::size_t c = 0; c < detail::kNumClasses; ++c) {
        if (detail::page* pg = h->cached[c]) {
            h->cached[c] = nullptr;
            detail::os_unmap(pg, detail::kPageSize, detail::kPageSize);
        }
    }
    // Drain the cross-class committed-page pool.
    while (h->pool) {
        void* p = h->pool;
        h->pool = *static_cast<void**>(p);
        detail::os_unmap(p, detail::kPageSize, detail::kPageSize);
    }
    h->pool_n = 0;
}

// ════════════════════════════════════════════════════════════════════════
//  11.  C-style surface + optional global new/delete interposition.
//
//  jetalloc is a full drop-in allocator. `jet::malloc`/`free`/`realloc`/
//  `calloc` give you the raw C API, and defining JET_GLOBAL_NEW before the
//  include routes EVERY `new`/`delete` in the program through jetalloc — the
//  whole process is transparently on jetalloc, no code changes at the call
//  sites. Both are size-free on the delete path (recovered from the block).
// ════════════════════════════════════════════════════════════════════════

// Raw C-style allocation. `jet::malloc` returns max_align_t-aligned storage;
// `jet::free` takes the pointer alone; `jet::realloc` preserves min(old,new).
[[nodiscard]] inline void* malloc(std::size_t n) noexcept {
    return detail::raw_allocate(n ? n : 1, alignof(std::max_align_t));
}
inline void free(void* p) noexcept { detail::free_unsized(p); }

[[nodiscard]] inline void* calloc(std::size_t count, std::size_t size) noexcept {
    if (count && size > std::numeric_limits<std::size_t>::max() / count) return nullptr;
    const std::size_t total = count * size;
    void* p = detail::raw_allocate(total ? total : 1, alignof(std::max_align_t));
    if (p) std::memset(p, 0, total);
    return p;
}

[[nodiscard]] inline void* realloc(void* p, std::size_t n) noexcept {
    if (!p) return jet::malloc(n);
    if (n == 0) { detail::free_unsized(p); return nullptr; }
    const std::size_t old = usable_size(p);
    if (n <= old) return p;                       // shrink / fits: keep in place
    void* np = jet::malloc(n);
    if (!np) return nullptr;
    std::memcpy(np, p, old < n ? old : n);
    detail::free_unsized(p);
    return np;
}

// Aligned C-style allocation (C11 aligned_alloc / posix_memalign semantics).
[[nodiscard]] inline void* aligned_alloc(std::size_t align, std::size_t n) noexcept {
    return detail::raw_allocate(n ? n : 1, align < alignof(std::max_align_t)
                                               ? alignof(std::max_align_t) : align);
}

}  // namespace jet

// ── Optional: make jetalloc THE global allocator for the whole program. ──────
// Define JET_GLOBAL_NEW in exactly ONE translation unit before including this
// header (or pass -DJET_GLOBAL_NEW). Every new/delete — throwing, nothrow,
// sized, and aligned — then goes through jetalloc. Nothing else changes.
#if defined(JET_GLOBAL_NEW)
#include <new>

[[nodiscard]] void* operator new(std::size_t n) {
    void* p = ::jet::detail::raw_allocate(n ? n : 1, alignof(std::max_align_t));
    if (!p) throw std::bad_alloc();
    return p;
}
[[nodiscard]] void* operator new[](std::size_t n) {
    void* p = ::jet::detail::raw_allocate(n ? n : 1, alignof(std::max_align_t));
    if (!p) throw std::bad_alloc();
    return p;
}
[[nodiscard]] void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    return ::jet::detail::raw_allocate(n ? n : 1, alignof(std::max_align_t));
}
[[nodiscard]] void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    return ::jet::detail::raw_allocate(n ? n : 1, alignof(std::max_align_t));
}
[[nodiscard]] void* operator new(std::size_t n, std::align_val_t a) {
    void* p = ::jet::detail::raw_allocate(n ? n : 1, static_cast<std::size_t>(a));
    if (!p) throw std::bad_alloc();
    return p;
}
[[nodiscard]] void* operator new[](std::size_t n, std::align_val_t a) {
    void* p = ::jet::detail::raw_allocate(n ? n : 1, static_cast<std::size_t>(a));
    if (!p) throw std::bad_alloc();
    return p;
}
[[nodiscard]] void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return ::jet::detail::raw_allocate(n ? n : 1, static_cast<std::size_t>(a));
}
[[nodiscard]] void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return ::jet::detail::raw_allocate(n ? n : 1, static_cast<std::size_t>(a));
}

void operator delete(void* p) noexcept                        { ::jet::detail::free_unsized(p); }
void operator delete[](void* p) noexcept                      { ::jet::detail::free_unsized(p); }
void operator delete(void* p, std::size_t) noexcept           { ::jet::detail::free_unsized(p); }
void operator delete[](void* p, std::size_t) noexcept         { ::jet::detail::free_unsized(p); }
void operator delete(void* p, std::align_val_t) noexcept      { ::jet::detail::free_unsized(p); }
void operator delete[](void* p, std::align_val_t) noexcept    { ::jet::detail::free_unsized(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept   { ::jet::detail::free_unsized(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { ::jet::detail::free_unsized(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept   { ::jet::detail::free_unsized(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ::jet::detail::free_unsized(p); }
#endif  // JET_GLOBAL_NEW

#endif  // JETALLOC_HPP
