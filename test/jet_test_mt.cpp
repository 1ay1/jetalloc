// jet_test_mt.cpp — multithreaded stress: producer/consumer cross-thread frees,
// the exact pattern the per-page atomic remote-free list must make safe.
// A miscompiled remote path corrupts the heap and this crashes or mismatches.
// SPDX-License-Identifier: MIT
#include "jetalloc.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace jet;

// A lock-free-ish MPMC-ish hand-off via a mutex-free sharded queue would be
// ideal, but for a correctness stress a simple mutexed queue per consumer is
// fine — the allocator's cross-thread path is what we're testing, not the queue.
#include <mutex>
#include <queue>

struct Block { void* p; std::size_t sz; };

int main() {
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr std::size_t kPerProducer = 200000;

    std::mutex m;
    std::queue<Block> q;
    std::atomic<bool> done{false};
    std::atomic<std::size_t> freed{0};

    std::vector<std::thread> producers, consumers;

    for (int t = 0; t < kProducers; ++t) {
        producers.emplace_back([&, t] {
            std::size_t seed = 0x9e3779b9u * (t + 1);
            for (std::size_t i = 0; i < kPerProducer; ++i) {
                seed = seed * 6364136223846793005ull + 1;
                std::size_t sz = 8 + (seed >> 40) % 2048;     // 8..2KB
                void* p = detail::raw_allocate(sz, 16);
                std::memset(p, 0xCD, sz);                     // touch the memory
                std::lock_guard<std::mutex> lk(m);
                q.push({p, sz});
            }
        });
    }

    for (int t = 0; t < kConsumers; ++t) {
        consumers.emplace_back([&] {
            for (;;) {
                Block b{nullptr, 0};
                {
                    std::lock_guard<std::mutex> lk(m);
                    if (!q.empty()) { b = q.front(); q.pop(); }
                }
                if (b.p) {
                    // free on a DIFFERENT thread than allocated → remote path
                    detail::raw_deallocate(b.p, b.sz, 16);
                    freed.fetch_add(1, std::memory_order_relaxed);
                } else if (done.load(std::memory_order_acquire)) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& t : producers) t.join();
    done.store(true, std::memory_order_release);
    for (auto& t : consumers) t.join();

    // Drain any stragglers (queue could hold items when consumers exited).
    while (!q.empty()) { auto b = q.front(); q.pop();
                         detail::raw_deallocate(b.p, b.sz, 16); freed.fetch_add(1); }

    std::size_t expected = std::size_t(kProducers) * kPerProducer;
    bool ok = freed.load() == expected;
    std::printf("jet_test_mt: %zu/%zu blocks freed cross-thread — %s\n",
                freed.load(), expected, ok ? "OK" : "MISMATCH");
    return ok ? 0 : 1;
}
