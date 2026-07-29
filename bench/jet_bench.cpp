// jet_bench.cpp — throughput benchmark: jetalloc vs the system allocator on the
// SAME workloads, through the same typed handles. Reports Mops/s (higher =
// better). Build -O2. SPDX-License-Identifier: MIT
#include "jetalloc.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using clk = std::chrono::steady_clock;
static double mops(std::size_t ops, clk::duration d) {
    double s = std::chrono::duration<double>(d).count();
    return (ops / 1e6) / s;
}

static volatile void* g_sink;

// Opaque, never-inlined system alloc/free so the compiler cannot elide the
// malloc/free pair (GCC/Clang recognise same-size malloc+free as dead code).
__attribute__((noinline)) static void* sys_alloc(std::size_t n) { return std::malloc(n); }
__attribute__((noinline)) static void  sys_free(void* p, std::size_t) { std::free(p); }

// ── small-fixed: allocate + free a 32 B block in a tight loop ──────────
template <class Alloc, class Free>
static double small_fixed(std::size_t iters, Alloc a, Free f) {
    auto t0 = clk::now();
    for (std::size_t i = 0; i < iters; ++i) {
        void* p = a(32);
        g_sink = p;          // force the allocation to be observed
        *static_cast<char*>(p) = static_cast<char>(i);   // touch the memory
        f(p, 32);
    }
    return mops(iters, clk::now() - t0);
}

// ── mixed-size: a working set of live blocks, random 8 B–4 KiB, churned ────
template <class Alloc, class Free>
static double mixed_size(std::size_t iters, Alloc a, Free f) {
    constexpr std::size_t N = 4096;
    std::vector<std::pair<void*, std::size_t>> live(N, {nullptr, 0});
    std::mt19937 rng(12345);
    std::uniform_int_distribution<std::size_t> sz(8, 4096), idx(0, N - 1);
    auto t0 = clk::now();
    for (std::size_t i = 0; i < iters; ++i) {
        std::size_t j = idx(rng);
        if (live[j].first) f(live[j].first, live[j].second);
        std::size_t s = sz(rng);
        live[j] = {a(s), s};
    }
    for (auto& [p, s] : live) if (p) f(p, s);
    return mops(iters, clk::now() - t0);
}

int main() {
    constexpr std::size_t ITERS = 5'000'000;

    auto jet_a = [](std::size_t n){ return jet::detail::raw_allocate(n, 16); };
    auto jet_f = [](void* p, std::size_t n){ jet::detail::raw_deallocate(p, n, 16); };
    auto sys_a = [](std::size_t n){ return sys_alloc(n); };
    auto sys_f = [](void* p, std::size_t n){ sys_free(p, n); };

    std::printf("jetalloc benchmark — %zu iterations, Mops/s (higher=better)\n\n", ITERS);
    std::printf("%-18s %12s %12s %10s\n", "workload", "jetalloc", "system", "speedup");

    double j1 = small_fixed(ITERS, jet_a, jet_f);
    double s1 = small_fixed(ITERS, sys_a, sys_f);
    std::printf("%-18s %12.1f %12.1f %9.2fx\n", "small-fixed(32B)", j1, s1, j1 / s1);

    double j2 = mixed_size(ITERS, jet_a, jet_f);
    double s2 = mixed_size(ITERS, sys_a, sys_f);
    std::printf("%-18s %12.1f %12.1f %9.2fx\n", "mixed-size(8-4K)", j2, s2, j2 / s2);

    return 0;
}
