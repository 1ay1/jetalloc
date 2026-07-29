<div align="center">

```
       ██╗███████╗████████╗ █████╗ ██╗     ██╗      ██████╗  ██████╗
       ██║██╔════╝╚══██╔══╝██╔══██╗██║     ██║     ██╔═══██╗██╔════╝
       ██║█████╗     ██║   ███████║██║     ██║     ██║   ██║██║
  ██   ██║██╔══╝     ██║   ██╔══██║██║     ██║     ██║   ██║██║
  ╚█████╔╝███████╗   ██║   ██║  ██║███████╗███████╗╚██████╔╝╚██████╗
   ╚════╝ ╚══════╝   ╚═╝   ╚═╝  ╚═╝╚══════╝╚══════╝ ╚═════╝  ╚═════╝
```

### the allocator where memory bugs don't compile

**A header-only, type-theoretic, memory-safe *and* thread-safe allocator for modern C++.**

![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=c%2B%2B&logoColor=white)
![header-only](https://img.shields.io/badge/header--only-2%20files-success)
![license](https://img.shields.io/badge/license-MIT-blue)
![platforms](https://img.shields.io/badge/platforms-Linux%20%C2%B7%20macOS%20%C2%B7%20Windows%20%C2%B7%20BSD-lightgrey)
![deps](https://img.shields.io/badge/dependencies-0-brightgreen)

</div>

---

**Two headers. No `.c`, no assembly, no linking, no dependencies.** `#include <jetalloc.hpp>`
and go. Under the hood it's a fast slab allocator — per-thread heaps, 64 KiB
header-free slab pages, lock-free size-class free lists, pages straight from the
kernel. On the *surface* it's something rarer: an allocator whose **C++ type
system makes memory bugs unrepresentable**, backed by a proof suite that asserts
the unsafe programs *fail to compile*.

The bar it sets for itself is one sentence: *a Rust programmer should read the
API and recognise the guarantees.* Then find out they're enforced without a
borrow checker in the language — by the type system alone.

```cpp
#include <jetalloc.hpp>
using namespace jet;

auto p = make<int>(42);        // owned<int>  — affine, RAII, provably ours
auto r = p.borrow();           // ref<int>    — &int   (shared, many allowed)
auto m = p.borrow_mut();       //             — &mut   ✗ PANICS: r is still live
// ... p's destructor frees. No delete. No double free. No leak. No UAF.

owned<int> q = std::move(p);   // p is now null — moved-from, statically dead
int bad = *p;                  // ✗ won't compile: owned<T> has no operator*
```

### The bug that doesn't compile

```cpp
owned<int> a = make<int>(1);
owned<int> b = a;              // ← this line does not compile.
```
```
error: use of deleted function 'jet::owned<int>::owned(const owned<int>&)'
        owned<int> b = a;
                       ^
note: 'owned' is affine — copying it would create a second owner, i.e. a double free.
```

That's not a lint or a runtime check. It is the C++ type system refusing to
represent a second owner. The whole library is built this way: **the mistake has
no syntax.**

## Why

A `malloc`/`free` interface can be *fast*, but it can never be *safe*: it hands
you a `void*` and trusts you to free it once, with the right size, and never
touch it again. Those are runtime obligations the compiler cannot see.

jetalloc lifts every one of those obligations into a **type**, so the compiler
(or, where the language can't express the check statically, a single defined
runtime trap) enforces it — the heap is never left to corrupt.

| Rust concept | jetalloc encoding |
|---|---|
| Ownership / `Drop` | `owned<T>` / `owned_array<T>` — **affine** (move-only) RAII handles. No copy ctor ⇒ **no double free**. Move nulls the source ⇒ **no use-after-move**. Destructor drops ⇒ **no leak**. |
| `&T` (shared borrow) | `ref<T>` — read-only, copyable, many allowed. |
| `&mut T` (unique borrow) | `mut<T>` — exclusive, move-only, at most one. |
| Aliasing XOR (`&` many **xor** `&mut` one) | enforced by a per-object **borrow ledger**: a violation is a loud `panic()`, never UB — the borrow checker, done dynamically with identical semantics. |
| Lifetimes | a borrow pins its owner; dropping an owner with a live borrow **panics** (dangling-borrow trap). |
| `Result<T, E>` | `expected<owned<T>, alloc_error>` — OOM is a **value**; `[[nodiscard]]` forces you to handle it. |
| `&[T]` (bounded slice) | `owned_array<T>` yields a `std::span`; `.at()` is bounds-checked. |
| provenance | you **cannot** build an `owned<T>` from a raw pointer — the only mint site is the allocator, so "did jetalloc allocate this?" is a **theorem**, not a runtime query. |

## The guarantees, and how each is enforced

- **Double free** — impossible. `owned<T>` has a deleted copy constructor; there
  is no second handle to free through.
- **Use-after-move** — a moved-from handle is null; its destructor is a no-op
  and any borrow attempt panics.
- **Use-after-free** — freeing consumes the handle by value; there is no name
  left to dereference.
- **Wrong-size free** — the element count lives *in* the handle, so the sized
  free is computed, never passed.
- **Buffer overrun** — array elements are reached only through a bounded
  `std::span` or a checked `.at()`; raw pointer arithmetic is not exposed.
- **Bad alignment** — alignment is a compile-time `power_of_two<N>` proof;
  `power_of_two<48>` does not instantiate.
- **Data race on a single object** — the aliasing-XOR borrow ledger forbids a
  `&mut` coexisting with any `&`.
- **Silent OOM** — the primary API returns `jet::result<…>` (`std::expected`);
  the throwing `make<…>` sits on top for callers who prefer exceptions.
- **Leak** — every handle is RAII; there is no manual free to forget.

The dangerous operations don't merely fail at runtime — **they don't compile.**
The test suite proves it: `test/neg/` contains programs that *must* fail to
compile, and CTest asserts the compiler rejects each one.

```
$ ctest --output-on-failure
 jet_test .............   Passed   # runtime behaviour + static_assert proofs
 jet_test_mt ..........   Passed   # 4×4 producer/consumer cross-thread-free stress
 jet_test_sync ........   Passed   # concurrency guarantees (guarded / channel)
 neg_double_free ......   Passed   # copying an owned<T> does NOT compile
 neg_wrong_align ......   Passed   # power_of_two<48> does NOT compile
 neg_copy_owned .......   Passed   # copying a borrow does NOT compile
 neg_non_send_thread ..   Passed   # sending a !Send type to a thread does NOT compile
 neg_copy_guard .......   Passed   # copying a lock guard does NOT compile
 neg_bad_lock_order ...   Passed   # taking locks out of rank order does NOT compile
 jet_tour .............   Passed   # the guided demo runs clean
100% tests passed
```

## Try it in 30 seconds

```sh
git clone --recursive <this-repo> && cd jetalloc
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/jet_tour        # a self-narrating tour of every guarantee
ctest --test-dir build  # runs the proofs, incl. the programs that must NOT compile
```

`examples/tour.cpp` is the whole library on one screen — and its last three
lines are the bugs that don't compile. Uncomment one and watch the type system
reject it.

## API tour

```cpp
using namespace jet;

// ── single objects ───────────────────────────────────────────────────────
result<owned<Widget>> w = try_make<Widget>(args...);   // OOM as a value
owned<Widget> x = make<Widget>(args...);               // or throwing

x.with_mut([](Widget& w){ w.tick(); });                // scoped &mut
int v = x.with([](const Widget& w){ return w.value; });// scoped &

// ── arrays (bounded) ─────────────────────────────────────────────────────
auto a = make_array_with<int>(5, [](std::size_t i){ return int(i*i); });
for (int n : a.view()) use(n);        // std::span, no raw pointer escapes
int last = a.at(4);                   // bounds-checked; a.at(5) throws

// ── raw bytes, still bounded + affine ────────────────────────────────────
auto buf = buffer::make(4096);                    // std::span<std::byte>
auto aln = buffer::make(256, power_of_two<64>{}); // over-aligned, proven

// ── drop-in std allocator ────────────────────────────────────────────────
std::vector<int, jet::allocator<int>> vec;        // container uses jetalloc
```

## Using it

Header-only — vendor the header, or link the CMake INTERFACE target:

```cmake
add_subdirectory(jetalloc)
target_link_libraries(your_app PRIVATE jetalloc::jetalloc)  # adds the include + C++23
```

Requires **C++23** (`std::expected`, `std::span`, concepts, `consteval`). To
disable the runtime borrow/contract checks in a hardened release build, compile
with `-DJET_NO_CONTRACTS` (you keep every *compile-time* guarantee; you lose
only the dynamic aliasing/dangling traps).

### Make the whole program run on jetalloc

jetalloc is a full drop-in allocator. Define `JET_GLOBAL_NEW` in **one**
translation unit and every `new`/`delete` in the process — throwing, nothrow,
sized, `new[]`, and over-aligned (`align_val_t`) — routes through jetalloc. No
call sites change; `std::vector`, `std::string`, `std::make_shared`, and every
other container come along for free.

```cpp
#define JET_GLOBAL_NEW      // one TU, before the include (or -DJET_GLOBAL_NEW)
#include <jetalloc.hpp>
// ...every new/delete in the whole program is now jetalloc. Nothing else changes.
```

Prefer the C API? It's all here, size-free on the free path:

```cpp
void* p = jet::malloc(64);
p = jet::realloc(p, 4096);         // grows across size classes, preserves data
void* z = jet::calloc(256, 4);     // zeroed
void* a = jet::aligned_alloc(128, 512);
jet::free(p); jet::free(z); jet::free(a);   // pointer only — no size to pass
```

### Hardened mode (`-DJET_HARDENED`)

Memory-*safety* (the typed layer) stops **your** bugs at compile time. Memory-
*integrity* (this tier) defends the heap itself against corruption — the kind
that comes from a buffer overflow elsewhere in the process, or a stale pointer
freed twice. Compile with `-DJET_HARDENED` and the raw allocator gains, on the
free path:

- **Freelist-pointer encoding** — the `next` link a freed block stores is
  XOR-encoded with a per-page random cookie. The classic exploit primitive
  ("overflow into a freed block, overwrite its link, redirect the next
  allocation to an address you chose") is defeated: you'd have to know the
  cookie. A decoded link that doesn't point at a real block in the page is a
  **defined abort**, not a hijacked `malloc`.
- **Double-free & invalid-free detection** — freeing a pointer that isn't a
  real block start (a wild or interior pointer), or re-freeing a block already
  on the free list, aborts with a diagnostic instead of silently corrupting the
  list.

Every check is gated behind `if constexpr (kHardened)`, so a **normal build
emits byte-identical hot-path code** — you pay nothing unless you opt in. When
you do, the hardened `page-churn` workload still runs at ~3× system malloc.
Hardening is **orthogonal to `-DJET_NO_CONTRACTS`**: a heap-integrity violation
is always fatal and loud, even in a build with the type-layer contracts off
— a corrupted heap must never be allowed to proceed. Proven by
`test/jet_test_hardened.cpp`, which spawns the corruption cases and asserts each
aborts.

## Cross-platform, native speed everywhere

jetalloc talks to the OS through **one virtual-memory layer**, and every
platform branch is resolved at **compile time** (a single `#if`) so there is
zero portability overhead in the emitted code:

| Platform | Page source | Notes |
|---|---|---|
| Windows | `VirtualAlloc` / `VirtualFree` | 64 KiB allocation granularity == our slab-page size, so pages come back aligned with **no copy**. Only the two kernel32 entry points are declared — `<windows.h>` is never included. |
| Linux / macOS / *BSD / Haiku | `mmap` / `munmap` | slab pages come **straight from the kernel** — no libc-malloc arena in the middle, so jetalloc neither depends on nor contends with another allocator. `mmap` has no portable aligned variant, so we over-map by the alignment and `munmap` the head/tail slack (the jemalloc/mimalloc technique); large regions get a best-effort `MADV_HUGEPAGE` hint on Linux. |
| Anything else (freestanding) | `malloc` + align | portable fallback; still correct, just an extra pointer of bookkeeping. |

The hot path is tuned for the microarchitecture, portably: `[[likely]]`/branch
hints steer the predictor toward the same-thread fast path, the fast path is
`always_inline`, and the per-thread heap is reached through a cached
`thread_local` pointer so a run of allocations keeps it in a register. Slab
pages are **header-free** — a block's owning page is recovered by masking its
address — so same-thread `alloc`/`free` are a branchless pop/push on a
free list. The result is the same class of allocator as mimalloc/tcmalloc,
but header-only and memory-safe by construction.

### Speed, in the design

Every step of the fast path was engineered to be O(1) and syscall-free:

- **size → class** is a shift + one byte-table load (no division, no loop).
- **allocation** pops a free list or bumps a pointer — no page-list scan, because
  full pages are unlinked the instant they fill, so the list head is always
  allocatable.
- **page geometry** (data start, capacity) is computed once when a page is
  created, never on the hot path.
- **two tiers of syscall-free page reserve**: one empty page per size class is
  retained in place, and a small **cross-class pool of committed 64 KiB pages**
  absorbs churn that spills across classes. A grow-then-free-a-batch loop reuses
  a pooled page with a plain relink — **zero syscalls in steady state**.
- **`owned<T>` is a single allocation** — the borrow ledger is co-located with
  the object in the same block (and vanishes entirely under `-DJET_NO_CONTRACTS`).

**Why this matters most on Windows.** On Linux `mmap`/`munmap` are cheap; on
Windows every fresh slab page would otherwise be a `VirtualAlloc` kernel
transition and every freed page a `VirtualFree`. The committed page pool keeps
retired pages mapped **in-process**, so the hot churn path never crosses into
the kernel — the same trick mimalloc uses to be fast on Windows, specialised to
jetalloc's uniform 64 KiB pages. Measured effect below (`page-churn`): removing
the pool drops that workload from **9.4× to 1.6×** — a **5.5×** swing, all of it
saved syscalls. TLS is native (`%gs`-relative, no `__emutls` call) on the
toolchains we ship, so `t_heap` access is a single segment load.

Measured with `bench/jet_bench.cpp` (`-O2`, jetalloc vs. the platform allocator
through the identical typed interface; Mops/s, higher = better):

| Workload | jetalloc | system malloc | speedup |
|---|---:|---:|---:|
| small-fixed (32 B) | **65–114** | ~15 | **4–7×** |
| mixed-size (8 B–4 KiB) | **17–20** | ~8 | **~2.4×** |
| page-churn (batch grow/free) | **~69** | ~7 | **~9×** |

(Windows / UCRT, single core, `-O2`. Absolute Mops/s vary run-to-run and by
machine — reproduce them yourself with `./build/jet_bench`. The *ratio* is the
point, and it holds because the wins above are structural, not micro-tuning.)

## Thread safety

jetalloc is **fully thread-safe** and lock-free on the hot path. Each thread
owns a private heap; a page belongs to exactly one heap.

- **Same-thread `alloc`/`free`** touch only owner-private state — **zero atomics,
  zero locks**.
- **Cross-thread `free`** (you allocate on thread A, free on thread B — the
  norm for thread pools, work queues, and any container moved across threads)
  is handled the mimalloc/snmalloc way: the block is pushed onto the page's
  **lock-free MPSC `remote_free` stack** with a single `compare_exchange`, and
  the owning thread reclaims it lazily. No owner-private state is ever mutated
  from a foreign thread, so there is **no data race** — verified by a 4×4
  producer/consumer stress test (`test/jet_test_mt.cpp`) and clean under
  ThreadSanitizer in CI.

## Introspection & maintenance

```cpp
jet::version();               // "0.3.0"
jet::usable_size(p);          // real block size for a jetalloc pointer
jet::trim();                  // return this thread's cached empty pages to the OS
```

## Production status

- **Thread-safe**, including the cross-thread-free path (lock-free).
- **No leaks / no UB**: CI runs the full suite under AddressSanitizer +
  UndefinedBehaviorSanitizer and ThreadSanitizer on every push.
- **CI matrix**: Linux (GCC), macOS (Clang), Windows (MSVC), Debug + Release.
- **Guarantees are tested**: `test/neg/` contains programs that *must fail to
  compile*, and CTest asserts the compiler rejects each. The global-allocator
  interposition and C API are covered by `test/jet_test_global.cpp`.
- **Semantic versioning** via `JETALLOC_VERSION_*` macros.

## Concurrency where thread bugs don't compile (`jetalloc_sync.hpp`)

The allocator is thread-safe internally; the companion header
`<jetalloc_sync.hpp>` goes further and makes the *caller's* concurrency bugs
unrepresentable, by lifting the rules Rust enforces in its compiler into C++
concepts and types.

```cpp
#include <jetalloc_sync.hpp>
using namespace jet;

// Data lives INSIDE the lock. There is no API that yields a bare `T&`, so
// "touch shared state without the lock" is not an expressible program.
guarded<std::vector<int>> shared;
shared.with([](auto& v){ v.push_back(1); });      // the only way in

// Structured threads: args must be `sendable`, the body can't leak an
// exception into std::terminate, and the thread joins on scope exit.
{
    scoped_thread worker([](owned<int> x){ /* owns x */ }, make<int>(7));
}   // <- joined here, automatically

// Ownership hand-off with no aliasing — the value MOVES across the boundary.
channel<owned<Job>> jobs;
jobs.send(make<Job>(...));                        // sender no longer owns it
```

| Bug | Encoding | Caught |
|---|---|---|
| **Data race on shared state** | `guarded<T>` — T reachable only through a move-only, `[[nodiscard]]` lock guard | structurally: no bare `T&` exists |
| **Sharing a thread-unsafe type** | `sendable` / `shareable` concepts gate `scoped_thread` args | **compile error** at the spawn site |
| **Deadlock (ABBA lock order)** | `guarded<T, Rank>` + `assert_lock_order<Outer, Inner>()` | **compile error** (same scope) / debug tripwire (across functions) |
| **Aliasing lock ownership** | the lock guard is move-only, non-copyable | **compile error** |
| **Forgotten join / detached-thread `terminate`** | `scoped_thread` joins on destruction; body wraps exceptions | structurally unreachable |
| **Use-after-move of a sent value** | `channel::send` / `scoped_thread` take by move; the source is emptied | affine handle: no name left |

These are proved by `test/neg/{non_send_thread,copy_guard,bad_lock_order}.cpp`
— programs that **must fail to compile**, asserted by CTest.

## License

MIT — see [LICENSE](LICENSE).
