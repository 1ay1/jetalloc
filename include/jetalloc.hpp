// jetalloc.hpp — modern C++ interface to jetalloc.
//
// jetalloc is, first and foremost, a transparent global allocator: link
// libjetalloc and every malloc/new in the program routes through it with
// NOTHING to include. This header is for the cases where you want to touch the
// allocator EXPLICITLY and idiomatically from C++23:
//
//   • jet::scoped_trim / jet::with_trim   — RAII page reclamation
//   • jet::stats() / jet::version()       — typed introspection
//   • jet::allocator<T>                   — a std-conformant allocator that
//                                           binds a container to jetalloc by
//                                           TYPE (independent of the global
//                                           override; deterministic + testable)
//   • jet::memory_resource()              — a std::pmr resource on jetalloc
//   • jet::allocate_unique<T>(...)        — unique_ptr explicitly on jetalloc
//   • jet::owns(p)                        — safe foreign-pointer query
//
// C++23 is the target; every use of a post-17 facility is feature-gated so the
// header still compiles cleanly at C++17 (the library's own default standard),
// degrading only in ergonomics (no concept constraints), never in behaviour.
//
// SPDX-License-Identifier: MIT
#ifndef JETALLOC_HPP
#define JETALLOC_HPP

#include "jetalloc.h"   // the C ABI this is a thin, zero-overhead skin over

#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <version>

#if defined(__cpp_lib_memory_resource) && __cpp_lib_memory_resource >= 201603L
#  include <memory_resource>
#  define JET_HAS_PMR 1
#else
#  define JET_HAS_PMR 0
#endif

#if defined(__cpp_concepts) && __cpp_concepts >= 201907L
#  define JET_REQUIRES(...) requires (__VA_ARGS__)
#else
#  define JET_REQUIRES(...)
#endif

namespace jet {

// ── Thin typed wrappers over the C entry points ──────────────────────────

[[nodiscard]] inline void* malloc(std::size_t n) noexcept { return ::jet_malloc(n); }
inline void free(void* p) noexcept { ::jet_free(p); }
inline void free_sized(void* p, std::size_t n) noexcept { ::jet_free_sized(p, n); }
[[nodiscard]] inline void* calloc(std::size_t count, std::size_t size) noexcept {
    return ::jet_calloc(count, size);
}
[[nodiscard]] inline void* realloc(void* p, std::size_t n) noexcept { return ::jet_realloc(p, n); }
[[nodiscard]] inline void* aligned_alloc(std::size_t align, std::size_t n) noexcept {
    return ::jet_aligned_alloc(align, n);
}

// Usable size of an allocation (0 for a pointer jetalloc did not hand out).
[[nodiscard]] inline std::size_t usable_size(const void* p) noexcept {
    return ::jet_usable_size(p);
}

// True iff `p` was allocated by jetalloc. Constant-time; NEVER dereferences p,
// so it is safe on a foreign or dangling pointer.
[[nodiscard]] inline bool owns(const void* p) noexcept { return ::jet_owns(p) != 0; }

// Version string, e.g. "0.1.0".
[[nodiscard]] inline const char* version() noexcept { return ::jet_version(); }

// Return cached free pages to the OS. Cheap; call at a quiescent boundary.
inline void trim() noexcept { ::jet_trim(); }

// ── stats(): a typed, aggregate-initialisable snapshot ───────────────────

struct stats_t {
    std::size_t bytes_mapped = 0;  // virtual memory reserved from the OS
    std::size_t bytes_live   = 0;  // bytes currently handed out
    std::size_t pages_active = 0;  // live 64 KiB slab pages
    std::size_t large_active = 0;  // live large (direct-mmap) allocations
    std::size_t alloc_calls  = 0;  // lifetime alloc calls (only if built -DJET_STATS)
    std::size_t free_calls   = 0;  // lifetime free  calls (only if built -DJET_STATS)
};

[[nodiscard]] inline stats_t stats() noexcept {
    ::jet_stats s{};
    ::jet_get_stats(&s);
    return stats_t{s.bytes_mapped, s.bytes_live, s.pages_active,
                   s.large_active, s.alloc_calls, s.free_calls};
}

// ── scoped_trim: RAII page reclamation ───────────────────────────────────
//
//     void handle(const Request& r) {
//         jet::scoped_trim _;               // trims on return OR throw
//         ... transient allocation ...
//     }
class scoped_trim {
public:
    scoped_trim() noexcept = default;
    ~scoped_trim() { ::jet_trim(); }

    scoped_trim(const scoped_trim&)            = delete;
    scoped_trim& operator=(const scoped_trim&) = delete;
    scoped_trim(scoped_trim&&)                 = delete;
    scoped_trim& operator=(scoped_trim&&)      = delete;
};

// Invoke fn, then trim; perfect-forwards fn's result (works for void too).
template <class F>
decltype(auto) with_trim(F&& fn) {
    scoped_trim guard;
    return std::forward<F>(fn)();
}

// ── allocator<T>: a std-conformant, stateless allocator on jetalloc ───────
//
// Binds a container to jetalloc BY TYPE rather than via the global new
// override. Reasons to want that:
//   • Determinism/testability — jet::owns() on the storage is guaranteed true
//     regardless of how the program was linked.
//   • Portability — on a toolchain without the global override (e.g. MSVC),
//     the container's storage still routes through jetalloc when the C symbols
//     are linked.
//
//     std::vector<int, jet::allocator<int>> v;                 // storage is jetalloc's
//     std::basic_string<char, std::char_traits<char>, jet::allocator<char>> s;
//
// Stateless ⇒ all instances compare equal ⇒ storage is freely interchangeable
// across copies (the Allocator requirement).
template <class T>
class allocator {
public:
    using value_type                             = T;
    using size_type                              = std::size_t;
    using difference_type                        = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal                        = std::true_type;

    template <class U> struct rebind { using other = allocator<U>; };

    constexpr allocator() noexcept = default;
    template <class U> constexpr allocator(const allocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t n)
        JET_REQUIRES(!std::is_const_v<T>)
    {
        if (n > max_size()) throw std::bad_alloc();
        void* p = nullptr;
        // Honour over-aligned T (alignas). Default-aligned goes through the
        // fast jet_malloc path; stricter alignment through aligned_alloc.
        if constexpr (alignof(T) > alignof(std::max_align_t))
            p = ::jet_aligned_alloc(alignof(T), n * sizeof(T));
        else
            p = ::jet_malloc(n * sizeof(T));
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t n) noexcept {
        ::jet_free_sized(p, n * sizeof(T));   // sized free — the fast path
    }

    [[nodiscard]] constexpr size_type max_size() const noexcept {
        return std::numeric_limits<size_type>::max() / sizeof(T);
    }
};

template <class T, class U>
[[nodiscard]] constexpr bool operator==(const allocator<T>&, const allocator<U>&) noexcept {
    return true;
}
#if !(defined(__cpp_impl_three_way_comparison) && __cpp_impl_three_way_comparison >= 201907L)
template <class T, class U>
[[nodiscard]] constexpr bool operator!=(const allocator<T>&, const allocator<U>&) noexcept {
    return false;
}
#endif

// Convenience aliases for the two containers people reach for first.
template <class T> using vector = std::vector<T, allocator<T>>;
using string = std::basic_string<char, std::char_traits<char>, allocator<char>>;

// ── deleter / allocate_unique: unique_ptr explicitly on jetalloc ─────────

// Stateless deleter: destroys the object, then returns its storage to jetalloc
// with a sized free. Empty ⇒ unique_ptr<T, jet::deleter<T>> is pointer-sized.
template <class T>
struct deleter {
    void operator()(T* p) const noexcept {
        if (p) {
            p->~T();
            ::jet_free_sized(p, sizeof(T));
        }
    }
};

template <class T>
using unique_ptr = std::unique_ptr<T, deleter<T>>;

// Construct a T on jetalloc storage and own it via jet::unique_ptr. Strongly
// exception-safe: if the constructor throws, the raw storage is freed and the
// exception propagates.
template <class T, class... Args>
[[nodiscard]] unique_ptr<T> allocate_unique(Args&&... args)
    JET_REQUIRES(std::is_constructible_v<T, Args...>)
{
    void* raw = (alignof(T) > alignof(std::max_align_t))
                    ? ::jet_aligned_alloc(alignof(T), sizeof(T))
                    : ::jet_malloc(sizeof(T));
    if (!raw) throw std::bad_alloc();
    try {
        T* obj = ::new (raw) T(std::forward<Args>(args)...);
        return unique_ptr<T>(obj);
    } catch (...) {
        ::jet_free_sized(raw, sizeof(T));
        throw;
    }
}

// ── memory_resource(): a std::pmr resource backed by jetalloc ────────────
#if JET_HAS_PMR

namespace detail {
class jet_memory_resource final : public std::pmr::memory_resource {
    void* do_allocate(std::size_t bytes, std::size_t align) override {
        void* p = (align > alignof(std::max_align_t))
                      ? ::jet_aligned_alloc(align, bytes)
                      : ::jet_malloc(bytes);
        if (!p) throw std::bad_alloc();
        return p;
    }
    void do_deallocate(void* p, std::size_t bytes, std::size_t /*align*/) override {
        ::jet_free_sized(p, bytes);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        // Stateless: any jet_memory_resource is interchangeable with any other.
        return dynamic_cast<const jet_memory_resource*>(&other) != nullptr;
    }
};
} // namespace detail

// Process-wide jetalloc pmr resource (Meyers singleton — thread-safe init,
// never destroyed before program end).
[[nodiscard]] inline std::pmr::memory_resource* memory_resource() noexcept {
    static detail::jet_memory_resource r;
    return &r;
}

// Make jetalloc the DEFAULT pmr resource, so std::pmr containers that don't
// name a resource use it. Returns the previous default (restore if you must).
inline std::pmr::memory_resource* set_as_default_pmr() noexcept {
    return std::pmr::set_default_resource(memory_resource());
}

#endif // JET_HAS_PMR

} // namespace jet

#endif // JETALLOC_HPP
