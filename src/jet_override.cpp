/*
 * jetalloc — C++ operator new / delete interposition.
 * SPDX-License-Identifier: MIT
 *
 * Defines the full replaceable operator new/delete set (C++11..C++17,
 * including sized and aligned variants). Linking this TU into an executable
 * routes ALL C++ heap allocations through jetalloc. Unlike a naive override,
 * delete consults jet_owns() so a pointer from a foreign allocator is handled
 * safely rather than crashing.
 */
#include <cstddef>
#include <new>

#include "jetalloc.h"

namespace {
inline void* jet_new(std::size_t n) {
    // operator new must never return nullptr; loop through new_handler.
    for (;;) {
        void* p = jet_malloc(n ? n : 1);
        if (p) return p;
        std::new_handler h = std::get_new_handler();
        if (!h) throw std::bad_alloc();
        h();
    }
}
inline void* jet_new_aligned(std::size_t n, std::size_t al) {
    for (;;) {
        void* p = jet_aligned_alloc(al, n ? n : 1);
        if (p) return p;
        std::new_handler h = std::get_new_handler();
        if (!h) throw std::bad_alloc();
        h();
    }
}
inline void jet_del(void* p) noexcept {
    if (!p) return;
    if (jet_owns(p)) jet_free(p);
    // Foreign pointer: leak rather than crash. In a pure-override build every
    // C++ allocation came through us, so this branch is effectively dead; it
    // exists only to make mixed-allocator loads crash-proof.
}
}  // namespace

/* ── throwing new ─────────────────────────────────────────────────────── */
void* operator new(std::size_t n)               { return jet_new(n); }
void* operator new[](std::size_t n)             { return jet_new(n); }

/* ── nothrow new ──────────────────────────────────────────────────────── */
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    return jet_malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    return jet_malloc(n ? n : 1);
}

/* ── aligned new (C++17) ──────────────────────────────────────────────── */
void* operator new(std::size_t n, std::align_val_t al) {
    return jet_new_aligned(n, static_cast<std::size_t>(al));
}
void* operator new[](std::size_t n, std::align_val_t al) {
    return jet_new_aligned(n, static_cast<std::size_t>(al));
}
void* operator new(std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept {
    return jet_aligned_alloc(static_cast<std::size_t>(al), n ? n : 1);
}
void* operator new[](std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept {
    return jet_aligned_alloc(static_cast<std::size_t>(al), n ? n : 1);
}

/* ── delete ───────────────────────────────────────────────────────────── */
void operator delete(void* p) noexcept              { jet_del(p); }
void operator delete[](void* p) noexcept            { jet_del(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept   { jet_del(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { jet_del(p); }

/* ── sized delete (C++14) — the fast delete path ──────────────────────── */
void operator delete(void* p, std::size_t sz) noexcept {
    if (p && jet_owns(p)) jet_free_sized(p, sz);
}
void operator delete[](void* p, std::size_t sz) noexcept {
    if (p && jet_owns(p)) jet_free_sized(p, sz);
}

/* ── aligned delete (C++17) ───────────────────────────────────────────── */
void operator delete(void* p, std::align_val_t) noexcept          { jet_del(p); }
void operator delete[](void* p, std::align_val_t) noexcept        { jet_del(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept   { jet_del(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { jet_del(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept   { jet_del(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { jet_del(p); }
