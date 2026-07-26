// jetalloc — C++ operator new/delete tests. SPDX-License-Identifier: MIT
#include "jetalloc.h"
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++fails; } } while (0)

int main() {
    std::printf("jetalloc %s — C++ tests\n", jet_version());

    // Plain new/delete
    int* i = new int(42);
    CHECK(*i == 42);
    CHECK(jet_owns(i));           // routed through jetalloc
    delete i;

    // new[]/delete[]
    auto* arr = new double[1000];
    for (int k = 0; k < 1000; ++k) arr[k] = k * 1.5;
    CHECK(arr[999] == 999 * 1.5);
    delete[] arr;

    // std containers (exercise sized-delete + realloc-ish growth)
    std::vector<std::string> v;
    for (int k = 0; k < 100000; ++k) v.push_back("item-" + std::to_string(k));
    CHECK(v.size() == 100000);
    CHECK(v.back() == "item-99999");

    std::unordered_map<int, std::string> m;
    for (int k = 0; k < 50000; ++k) m[k] = std::to_string(k * k);
    CHECK(m.size() == 50000);
    CHECK(m[123] == "15129");

    // aligned new (C++17 over-aligned type)
    struct alignas(64) Cache { char b[64]; };
    auto* c = new Cache();
    CHECK((reinterpret_cast<uintptr_t>(c) & 63) == 0);
    delete c;

    // shared_ptr / make_shared control-block allocation
    auto sp = std::make_shared<std::vector<int>>(10000, 7);
    CHECK(sp->at(9999) == 7);

    if (fails) { std::printf("\n%d CHECK(s) FAILED\n", fails); return 1; }
    std::printf("\nALL PASSED\n");
    return 0;
}
