// jet_test_global.cpp — proves jetalloc is a full drop-in global allocator.
// Defining JET_GLOBAL_NEW routes every new/delete in the program through
// jetalloc; this TU exercises that path plus the C-style malloc/realloc/free
// surface. SPDX-License-Identifier: MIT
#define JET_GLOBAL_NEW
#include "jetalloc.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

int main() {
    // ── raw new/delete now go through jetalloc ───────────────────────────────
    int* a = new int(7);
    assert(*a == 7);
    delete a;

    int* arr = new int[64];
    for (int i = 0; i < 64; ++i) arr[i] = i;
    assert(arr[63] == 63);
    delete[] arr;

    // ── over-aligned new (align_val_t overload) ──────────────────────────────
    struct alignas(64) Cache { char b[64]; };
    Cache* c = new Cache;
    assert((reinterpret_cast<std::uintptr_t>(c) % 64) == 0);
    delete c;

    // ── nothrow new ──────────────────────────────────────────────────────────
    int* n = new (std::nothrow) int(9);
    assert(n && *n == 9);
    delete n;

    // ── std containers transparently on jetalloc (no custom allocator) ───────
    std::vector<std::string> v;
    for (int i = 0; i < 2000; ++i) v.emplace_back(std::to_string(i) + "_payload");
    assert(v.size() == 2000);
    assert(v[1999] == "1999_payload");

    auto sp = std::make_shared<std::vector<int>>(1000, 42);
    assert(sp->at(999) == 42);

    // ── C-style surface: malloc / calloc / realloc / free ────────────────────
    char* m = static_cast<char*>(jet::malloc(100));
    assert(m);
    std::memset(m, 'x', 100);

    m = static_cast<char*>(jet::realloc(m, 4000));   // grow across size classes
    assert(m);
    for (int i = 0; i < 100; ++i) assert(m[i] == 'x'); // contents preserved
    jet::free(m);

    int* z = static_cast<int*>(jet::calloc(256, sizeof(int)));
    assert(z);
    for (int i = 0; i < 256; ++i) assert(z[i] == 0);   // zero-initialised
    jet::free(z);

    void* al = jet::aligned_alloc(128, 512);
    assert(al && (reinterpret_cast<std::uintptr_t>(al) % 128) == 0);
    jet::free(al);

    // ── realloc(nullptr) == malloc, realloc(p,0) == free ─────────────────────
    void* r = jet::realloc(nullptr, 32);
    assert(r);
    assert(jet::realloc(r, 0) == nullptr);

    std::puts("jet_test_global: all global-allocator + C-API checks passed");
    return 0;
}
