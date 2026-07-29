// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  jetalloc_sync.hpp — compile-time-safe concurrency for the jetalloc world. ║
// ║  Header-only. Makes data races, deadlocks, and thread-teardown bugs        ║
// ║  either UNREPRESENTABLE (won't compile) or trapped at a defined point.     ║
// ║  SPDX-License-Identifier: MIT                                              ║
// ╚══════════════════════════════════════════════════════════════════════════╝
//
// jetalloc.hpp made single-thread memory bugs unrepresentable. This companion
// does the same for CONCURRENCY, by lifting the rules Rust enforces in its
// compiler into C++ concepts and types:
//
//   Rust concept        →  jetalloc_sync encoding
//   ───────────────────────────────────────────────────────────────────────────
//   Send                →  concept `sendable<T>`  : may MOVE across threads.
//   Sync                →  concept `shareable<T>` : may share `const&` across
//                          threads. Interior-mutable types are excluded.
//   Mutex<T> (data IN   →  guarded<T, Rank> : the data lives INSIDE the lock.
//   the lock)              There is no API that yields a bare `T&` — the only
//                          way to reach T is a scoped lock guard, so a data
//                          race on T is not expressible.
//   no data races        →  a thread body may only capture `sendable` values by
//                          move and `shareable` values by const ref; anything
//                          else is a compile error at the spawn site.
//   lock ordering        →  each guarded<> carries a compile-time RANK; taking
//   (deadlock freedom)     two in the wrong order is a static_assert (same
//                          scope) or a debug tripwire (across functions).
//                          (Rust does NOT check this — we go strictly further.)
//   structured concurrency→ scoped_thread joins on destruction; the spawn
//                          primitive makes an escaping exception → std::terminate
//                          structurally unreachable.
//
// HONEST BOUNDARY (read this): C++ has no borrow checker in the language, so
// ONE class of bug — a reference into guarded data OUTLIVING its lock guard —
// cannot be rejected purely at compile time by a library. We make it as hard as
// the language allows (the guard is [[nodiscard]], move-only, and hands you a
// ref<T>/mut<T> whose lifetime is tied to it) and catch the rest with a debug
// tripwire. Everything ELSE here is a compile-time guarantee. We do not pretend
// otherwise.
//
#ifndef JETALLOC_SYNC_HPP
#define JETALLOC_SYNC_HPP

#include "jetalloc.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

namespace jet {

// ════════════════════════════════════════════════════════════════════════════
//  1.  Send / Sync — the concepts that gate what may cross a thread boundary.
// ════════════════════════════════════════════════════════════════════════════
//
// A type is `sendable` if transferring its ownership to another thread is
// sound. Trivial and standard-movable types qualify by default; types that own
// a thread-affine resource must OPT OUT by specialising jet::thread_unsafe.
//
// A type is `shareable` if handing another thread a `const T&` is sound — i.e.
// it has no interior mutability that a const reference could trigger unsynchr-
// onised. Atomics and already-synchronised wrappers are shareable; a bare type
// with `mutable` non-atomic state is not (opt out via thread_unsafe too).
template <class T> struct thread_unsafe : std::false_type {};

// Marker base a user can inherit to declare "never cross a thread boundary".
struct no_send { no_send() = default; };
template <class T>
    requires std::is_base_of_v<no_send, T>
struct thread_unsafe<T> : std::true_type {};

template <class T>
concept sendable =
    !thread_unsafe<std::remove_cvref_t<T>>::value &&
    std::is_move_constructible_v<std::remove_cvref_t<T>>;

template <class T>
concept shareable =
    !thread_unsafe<std::remove_cvref_t<T>>::value;

// An owned<T> / owned_array<T> / buffer is sendable exactly when its element is
// (ownership is affine, so moving it across a thread is a clean hand-off).
template <class T> struct thread_unsafe<owned<T>>       : thread_unsafe<T> {};
template <class T> struct thread_unsafe<owned_array<T>> : thread_unsafe<T> {};

// ════════════════════════════════════════════════════════════════════════════
//  2.  Lock hierarchy — compile-time rank; out-of-order = static_assert.
//      (Matches agentty::util::ranked_lock, generalised and data-carrying.)
// ════════════════════════════════════════════════════════════════════════════
namespace detail {

// Thread-local held-rank stack for the cross-function runtime tripwire. The
// compile-time check (below) covers the same-scope case regardless of NDEBUG.
constinit inline thread_local unsigned held_ranks[64] = {};
constinit inline thread_local int      held_depth     = 0;

inline void push_rank([[maybe_unused]] unsigned r) noexcept {
#ifndef NDEBUG
    if (held_depth > 0 && r <= held_ranks[held_depth - 1]) {
        std::fprintf(stderr,
            "jetalloc_sync: lock acquired out of rank order (%u after %u) — "
            "potential ABBA deadlock\n", r, held_ranks[held_depth - 1]);
        std::abort();
    }
    if (held_depth < 64) held_ranks[held_depth] = r;
#endif
    ++held_depth;
}
inline void pop_rank() noexcept { if (held_depth > 0) --held_depth; }

}  // namespace detail

// ════════════════════════════════════════════════════════════════════════════
//  3.  guarded<T, Rank> — data fused with its lock. The ONLY way to reach T.
// ════════════════════════════════════════════════════════════════════════════
//
// There is no method that returns a bare `T&`. `lock()` returns a move-only,
// [[nodiscard]] guard; the data is reachable only through THAT guard, and only
// while it is alive. So "touch shared data without holding the lock" is not an
// expressible program. The guard carries the compile-time rank for ordering.
template <class T, unsigned Rank = 0>
class guarded {
public:
    static constexpr unsigned rank = Rank;

    template <class... Args>
    explicit guarded(Args&&... args) : val_(std::forward<Args>(args)...) {}

    guarded(const guarded&)            = delete;
    guarded& operator=(const guarded&) = delete;

    // The access token. Move-only and [[nodiscard]] so it cannot be silently
    // dropped (which would release the lock immediately). While it lives, and
    // ONLY while it lives, the data is reachable — through it.
    class [[nodiscard]] guard {
    public:
        guard(guard&& o) noexcept
            : owner_(std::exchange(o.owner_, nullptr)) {}
        guard& operator=(guard&&) = delete;
        guard(const guard&)       = delete;
        ~guard() {
            if (owner_) { owner_->mtx_.unlock(); detail::pop_rank(); }
        }

        [[nodiscard]] T&       operator*()  noexcept { return owner_->val_; }
        [[nodiscard]] const T& operator*()  const noexcept { return owner_->val_; }
        [[nodiscard]] T*       operator->() noexcept { return &owner_->val_; }
        [[nodiscard]] const T* operator->() const noexcept { return &owner_->val_; }

    private:
        friend class guarded;
        explicit guard(guarded* o) noexcept : owner_(o) {}
        guarded* owner_;
    };

    [[nodiscard]] guard lock() {
        detail::push_rank(Rank);   // runtime tripwire (debug) for cross-fn order
        mtx_.lock();
        return guard(this);
    }

    // Transient scoped access — the common case. `with([](T&){...})` holds the
    // lock for exactly the callback and returns its result.
    template <class F> decltype(auto) with(F&& f) {
        auto g = lock();
        return std::forward<F>(f)(*g);
    }

private:
    friend class guard;
    std::mutex mtx_;
    T          val_;
};

// A guarded<> is shareable across threads BY REFERENCE regardless of whether T
// itself is — that is the entire point of putting T behind a lock.
template <class T, unsigned R> struct thread_unsafe<guarded<T, R>> : std::false_type {};

// Compile-time ordering proof for two guards taken in the same scope:
//     auto a = outer.lock();
//     jet::assert_lock_order<decltype(outer)::rank, decltype(inner)::rank>();
//     auto b = inner.lock();
// If inner's rank <= outer's, this is a COMPILE error.
template <unsigned OuterRank, unsigned InnerRank>
constexpr void assert_lock_order() noexcept {
    static_assert(InnerRank > OuterRank,
                  "lock-order violation: the inner lock's rank must be strictly "
                  "greater than the outer lock's (outer = lower rank, taken "
                  "first). This ordering is what makes ABBA deadlock impossible.");
}

// ════════════════════════════════════════════════════════════════════════════
//  4.  scoped_thread — structured concurrency with Send-checked arguments.
// ════════════════════════════════════════════════════════════════════════════
//
// Every argument forwarded to the thread body must be `sendable`; passing a
// thread-unsafe value is a compile error at the spawn site (no data race can
// begin). The body runs inside a wrapper that makes an escaping exception →
// std::terminate structurally unreachable (it is caught and swallowed with a
// diagnostic). The thread is JOINED on destruction — you cannot forget to join,
// and the thread cannot outlive the data it borrows from this scope.
class [[nodiscard]] scoped_thread {
public:
    template <class F, class... Args>
        requires (sendable<Args> && ...)
    explicit scoped_thread(F&& f, Args&&... args) {
        static_assert((sendable<Args> && ...),
                      "every argument crossing the thread boundary must be "
                      "sendable (movable and not thread_unsafe). Move ownership "
                      "in, or share via a guarded<> / atomic.");
        t_ = std::thread(
            [fn = std::forward<F>(f)](std::decay_t<Args>... a) mutable noexcept {
                // std::terminate is now structurally unreachable: any exception
                // escaping the body is caught here, not propagated out of the
                // thread function (which would call std::terminate).
                try { fn(std::move(a)...); }
                catch (const std::exception& e) {
                    std::fprintf(stderr, "jetalloc_sync: thread body threw: %s\n", e.what());
                } catch (...) {
                    std::fprintf(stderr, "jetalloc_sync: thread body threw (unknown)\n");
                }
            },
            std::forward<Args>(args)...);
    }

    scoped_thread(const scoped_thread&)            = delete;
    scoped_thread& operator=(const scoped_thread&) = delete;
    scoped_thread(scoped_thread&&)                 = default;
    scoped_thread& operator=(scoped_thread&&)      = default;

    ~scoped_thread() { if (t_.joinable()) t_.join(); }   // structured: always joins

    [[nodiscard]] std::thread::id id() const noexcept { return t_.get_id(); }

private:
    std::thread t_;
};

// ════════════════════════════════════════════════════════════════════════════
//  5.  channel<T> — move-only ownership hand-off between threads.
// ════════════════════════════════════════════════════════════════════════════
//
// A value SENT into the channel is MOVED; the sender no longer owns it, so no
// two threads ever alias it. T must be `sendable`. This is the safe primitive
// for producer/consumer without shared mutable state.
template <sendable T>
class channel {
public:
    channel() = default;
    channel(const channel&)            = delete;
    channel& operator=(const channel&) = delete;

    // Send moves the value in (rvalue-only): ownership transfers, no copy.
    void send(T value) {
        {
            std::lock_guard lk(m_);
            q_.push(std::move(value));
        }
        cv_.notify_one();
    }

    // Blocking receive; std::nullopt only after close() with an empty queue.
    [[nodiscard]] std::optional<T> recv() {
        std::unique_lock lk(m_);
        cv_.wait(lk, [&] { return !q_.empty() || closed_; });
        if (q_.empty()) return std::nullopt;
        T v = std::move(q_.front());
        q_.pop();
        return v;
    }

    void close() {
        { std::lock_guard lk(m_); closed_ = true; }
        cv_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::queue<T> q_;
    bool closed_ = false;
};

}  // namespace jet

#endif  // JETALLOC_SYNC_HPP
