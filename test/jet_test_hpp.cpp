// jetalloc — C++23 interface (jetalloc.hpp) tests. SPDX-License-Identifier: MIT
//
// Exercises the jet:: namespace surface: typed wrappers, scoped_trim,
// allocator<T>, allocate_unique, and the std::pmr resource. Every allocation
// made through jet::allocator / jet::allocate_unique / jet::memory_resource is
// asserted to be jetalloc-OWNED, which proves the header routes storage through
// the allocator regardless of how the global operator-new override linked.
#include "jetalloc.hpp"

#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#if JET_HAS_PMR
#  include <memory_resource>
#endif

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++fails; } } while (0)

// A type with strict over-alignment, to exercise the aligned paths.
struct alignas(64) CacheLine { char b[64]; };

int main() {
    std::printf("jetalloc %s — C++23 header tests (pmr=%d)\n",
                jet::version(), JET_HAS_PMR);

    // ── typed free-function wrappers + owns/usable_size ──────────────────
    {
        void* p = jet::malloc(128);
        CHECK(p != nullptr);
        CHECK(jet::owns(p));                    // ours
        CHECK(jet::usable_size(p) >= 128);
        int on_stack = 0;
        CHECK(!jet::owns(&on_stack));           // foreign, no deref, safe
        jet::free(p);
    }

    // ── jet::allocator<T> in a std::vector — storage must be jetalloc's ──
    {
        std::vector<int, jet::allocator<int>> v;
        for (int i = 0; i < 100000; ++i) v.push_back(i);
        CHECK(v.size() == 100000);
        CHECK(v.back() == 99999);
        CHECK(jet::owns(v.data()));             // the container's buffer is ours
    }

    // jet::vector / jet::string aliases
    {
        jet::vector<double> v(1000, 2.5);
        CHECK(v[999] == 2.5);
        CHECK(jet::owns(v.data()));

        jet::string s;
        for (int i = 0; i < 5000; ++i) s += 'x';   // force heap (past SSO)
        CHECK(s.size() == 5000);
        CHECK(jet::owns(s.data()));
    }

    // ── over-aligned allocator path ──────────────────────────────────────
    {
        std::vector<CacheLine, jet::allocator<CacheLine>> v(10);
        CHECK((reinterpret_cast<std::uintptr_t>(v.data()) & 63) == 0);
        CHECK(jet::owns(v.data()));
    }

    // ── allocate_unique: construct on jetalloc, RAII ownership ───────────
    {
        auto up = jet::allocate_unique<std::pair<int, std::string>>(7, "seven");
        CHECK(up->first == 7);
        CHECK(up->second == "seven");
        CHECK(jet::owns(up.get()));
        static_assert(sizeof(up) == sizeof(void*),
                      "jet::unique_ptr must be pointer-sized (empty deleter)");

        // over-aligned object via allocate_unique
        auto ac = jet::allocate_unique<CacheLine>();
        CHECK((reinterpret_cast<std::uintptr_t>(ac.get()) & 63) == 0);
        CHECK(jet::owns(ac.get()));
    }

    // ── strong exception-safety of allocate_unique ───────────────────────
    {
        struct Boom { Boom() { throw std::runtime_error("ctor"); } };
        bool threw = false;
        try { auto b = jet::allocate_unique<Boom>(); (void)b; }
        catch (const std::runtime_error&) { threw = true; }
        CHECK(threw);   // storage was freed on the throw path; no leak, no crash
    }

    // ── std::pmr resource backed by jetalloc ─────────────────────────────
#if JET_HAS_PMR
    {
        std::pmr::vector<std::pmr::string> v{jet::memory_resource()};
        for (int i = 0; i < 2000; ++i) v.emplace_back("pmr-item");
        CHECK(v.size() == 2000);
        CHECK(jet::owns(v.data()));             // pmr buffer is jetalloc's

        // resources compare equal (stateless)
        CHECK(*jet::memory_resource() == *jet::memory_resource());
    }
#endif

    // ── scoped_trim / with_trim ──────────────────────────────────────────
    {
        auto before = jet::stats();
        {
            jet::scoped_trim guard;
            std::vector<int, jet::allocator<int>> v(1 << 20, 1);   // ~4 MiB
            CHECK(jet::owns(v.data()));
        } // guard runs jet_trim() here
        auto after = jet::stats();
        // pages_active is always maintained (unlike the -DJET_STATS counters);
        // it must not have grown across the trimmed scope.
        CHECK(after.pages_active <= before.pages_active + 1);

        int r = jet::with_trim([] { return 42; });
        CHECK(r == 42);
    }

    if (fails) { std::printf("\n%d CHECK(s) FAILED\n", fails); return 1; }
    std::printf("\nALL PASSED\n");
    return 0;
}
