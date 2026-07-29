// jetalloc — type-theoretic C++ allocator tests. SPDX-License-Identifier: MIT
//
// Two kinds of proof:
//   1. static_assert — the DANGEROUS operations don't compile. Strongest test.
//   2. runtime asserts — the safe operations behave, and contract violations
//      trap loudly (checked in test/neg/ for the compile-time ones).
#include "jetalloc.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace jet;

// ═══ Compile-time (type-theoretic) guarantees ════════════════════════════
static_assert(!std::is_copy_constructible_v<owned<int>>,      "owned is affine (no double free)");
static_assert(!std::is_copy_assignable_v<owned<int>>,         "owned has no copy assign");
static_assert(std::is_nothrow_move_constructible_v<owned<int>>,"owned move is noexcept (source nulled)");
static_assert(!std::is_copy_constructible_v<owned_array<int>>,"owned_array is affine");
static_assert(!std::is_copy_constructible_v<buffer>,          "buffer is affine");
static_assert(!std::is_copy_constructible_v<mut<int>>,        "mut (&mut) is unique — move only");
static_assert(std::is_copy_constructible_v<ref<int>>,         "ref (&) is shareable");

static_assert(power_of_two<64>::value == 64);
static_assert(align_of<double> == alignof(double));
static_assert(storable<int> && storable<std::string> && !storable<void> && !storable<int&>);

// ═══ Runtime behaviour ═══════════════════════════════════════════════════
static int live = 0;
struct Tracked {
    int v;
    explicit Tracked(int x) : v(x) { ++live; }
    ~Tracked() { --live; }
    Tracked(const Tracked&) = delete;
};

int fails = 0;
#define CHECK(c) do { if(!(c)){ std::printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c); ++fails; } } while(0)

int main() {
    // owned<T>: make, borrow shared/unique, drop
    {
        auto o = make<Tracked>(42);
        CHECK(o.engaged());
        {
            auto a = o.borrow();
            auto b = o.borrow();                 // many shared borrows: fine
            CHECK(a->v == 42 && b->v == 42);
        }
        o.with_mut([](Tracked& t){ t.v = 7; });
        CHECK(o.borrow()->v == 7);
        CHECK(live == 1);
    }
    CHECK(live == 0);                            // dtor dropped it

    // move nulls the source ⇒ no double free / no use-after-move
    {
        auto a = make<Tracked>(1);
        auto b = std::move(a);
        CHECK(!a.engaged() && b.engaged());
        CHECK(live == 1);
    }
    CHECK(live == 0);

    // owned_array: bounded access + checked .at()
    {
        auto arr = make_array_with<int>(5, [](std::size_t i){ return int(i*i); });
        CHECK(arr.size() == 5 && arr.at(4) == 16);
        int sum = 0; for (int x : arr.view()) sum += x;
        CHECK(sum == 0+1+4+9+16);
        bool threw = false;
        try { (void)arr.at(5); } catch (const std::out_of_range&) { threw = true; }
        CHECK(threw);
    }

    // expected path: OOM is a value
    {
        result<owned<int>> r = try_make<int>(99);
        CHECK(r.has_value());
        CHECK(r->borrow_mut().get() != nullptr);
        CHECK(*r->borrow() == 99);
    }

    // buffer: bounded raw bytes, compile-time-proven alignment
    {
        auto buf = buffer::make(1024);
        for (auto& x : buf.bytes()) x = std::byte{0xAB};
        CHECK(std::to_integer<int>(buf.bytes()[512]) == 0xAB);
        auto over = buffer::make(256, power_of_two<64>{});
        CHECK(reinterpret_cast<std::uintptr_t>(over.bytes().data()) % 64 == 0);
    }

    // std::vector on jet::allocator
    {
        std::vector<int, allocator<int>> v;
        for (int i = 0; i < 100000; ++i) v.push_back(i);
        CHECK(v.back() == 99999);
    }

    if (fails) { std::printf("\n%d CHECK(s) FAILED\n", fails); return 1; }
    std::printf("jet_test: all %s\n", "safety properties hold");
    return 0;
}
