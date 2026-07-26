# jetalloc

**A fast, modern, drop-in memory allocator.**

jetalloc is a small (~700 LOC) thread-caching allocator in the mimalloc / tcmalloc
family: per-thread heaps, page-based slabs with **no per-object header**, sharded
free lists so the same-thread `malloc`/`free` fast paths touch **zero atomics and
zero locks**, and a lock-free MPSC stack for cross-thread frees.

It is a **drop-in replacement**: link it, or `LD_PRELOAD` it, and your program's
`malloc`/`free`/`new`/`delete` route through jetalloc — no code changes.

## Why

Because a system allocator upgrade should never break your binary. jetalloc is
self-contained (no external dependency, no fragile global `free()` interposition
that crashes during C++ locale init), and it's *fast*:

| Workload                | jetalloc       | glibc malloc | speedup |
|-------------------------|----------------|--------------|---------|
| small-fixed (32 B loop) | **253 Mops/s** | 235 Mops/s   | 1.08×   |
| mixed-size (8 B–4 KiB)  | **164 Mops/s** | 18 Mops/s    | 9.1×    |
| threaded (8 threads)    | 1030 Mops/s    | 1149 Mops/s  | 0.90×   |

*(GCC 16, x86-64, `bench/jet_bench.c` — run it yourself, numbers vary by CPU.)*

## Design

- **Thread-local heaps.** Each thread owns its pages. The alloc fast path is a
  single free-list pop; the free fast path is a single push. No atomics on the
  common same-thread path.
- **Per-class thread cache (fast bins).** In front of the pages, each heap keeps
  a small LIFO of recycled blocks per size class (like glibc's tcache /
  mimalloc's thread free list). Most `malloc`/`free` pairs hit the bin and never
  touch page bookkeeping at all; surplus is flushed back to pages in batches.
- **Bump allocation.** A fresh page hands out its never-used blocks by pure
  pointer arithmetic (`bump += size`) — no up-front freelist threading, so a new
  page dirties only the cache lines it actually serves.
- **64 KiB slab pages, header-free blocks.** A block's owning page is recovered
  by masking its address (`ptr & ~(64KiB-1)`), so blocks carry no bookkeeping
  bytes — denser cache lines than a classic sized-header malloc.
- **Sharded free lists** (the mimalloc trick). Each page has a `local_free`
  (owner-thread frees, no atomics) and an atomic `thread_free` (cross-thread
  frees, lock-free Treiber stack). The owner drains `thread_free` lazily, so
  neither hot path pays for the cross-thread case it isn't hitting.
- **39 size classes**, ≤ 12.5 % internal fragmentation, O(1) size→class map.
- **Large allocations** (> 32 KiB) go straight to `mmap`, page-aligned, with a
  one-page header so `free` / `usable_size` / `owns` are O(1).
- **Crash-proof interposition.** `free()` consults `jet_owns()` and forwards
  foreign pointers to the real libc `free` — so replacing the system allocator
  can't corrupt the heap on a pointer jetalloc didn't hand out.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build          # 3 suites: C, C++, multithread — all pass
./build/jet_bench               # jetalloc numbers
./build/jet_bench_system        # system-allocator baseline
```

## Use

**Explicit API** (`#include <jetalloc.h>`, link `libjetalloc.a`):

```c
void* p = jet_malloc(128);
jet_free(p);
```

**Whole-program C++ override** — link the static lib; every `new`/`delete`
(and internal `malloc`) routes through jetalloc:

```sh
cc myapp.c  -ljetalloc
c++ myapp.cpp -ljetalloc
```

**Zero-recompile, any program** — preload the shared lib:

```sh
LD_PRELOAD=./build/libjetalloc.so  ./your_program
```

## API

```c
void*  jet_malloc(size_t);
void   jet_free(void*);
void*  jet_calloc(size_t, size_t);
void*  jet_realloc(void*, size_t);
void*  jet_aligned_alloc(size_t align, size_t size);
int    jet_posix_memalign(void**, size_t align, size_t size);
void   jet_free_sized(void*, size_t);      /* fast path for C++ sized delete */
size_t jet_usable_size(const void*);
int    jet_owns(const void*);              /* is this pointer ours? O(1)     */
void   jet_get_stats(jet_stats*);          /* build with -DJET_STATS         */
```

## Status

v0.1.0 — correct (100 % of the test suite, incl. an 8-thread cross-thread-free
stress test) and faster than glibc on single-threaded workloads. The threaded
path and a per-thread large-object cache are the next optimisation targets.

Build with `-DJET_STATS=1` for live `jet_get_stats` counters (off by default so
the hot path stays atomic-free), or `-DJET_DEBUG` for internal invariant checks.

## License

MIT — see [LICENSE](LICENSE).
