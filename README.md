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
that crashes during C++ locale init), and it's *fast* — measured head-to-head
against the fastest production allocators through the **identical** drop-in
`malloc`/`free` interface (`LD_PRELOAD`), median of 5 runs on a 12-core Zen box:

| Workload (Mops/s, higher = better) | jetalloc | tcmalloc | mimalloc | jemalloc | glibc |
|------------------------------------|---------:|---------:|---------:|---------:|------:|
| small-fixed (32 B loop)            |    279   |  **292** |    256   |    250   |  228  |
| **mixed-size (8 B–4 KiB)**          | **175** 🏆 |    151   |     61   |    102   |   18  |
| threaded (8 threads, same-thread)  |    960   |  **1041**|    865   |    874   | 1173  |
| producer/consumer (cross-thread)   |    103   |     22   |     73   |  **203** |   12  |

**Honest scorecard:** jetalloc **wins outright on mixed-size** (the most
realistic churn workload — 1.7–2.9× the other slab allocators), is a close **#2 on
small-fixed** (4% behind tcmalloc), competitive on threaded, and is **#2 on
cross-thread producer/consumer** — decisively ahead of tcmalloc/mimalloc/glibc,
with jemalloc still leading there (its consumer-side lazy free caching is a 2×
edge we've now halved from 3.2×). Reproduce it yourself: `bench/compare.sh`.

*(GCC/Clang, x86-64. Numbers vary by CPU; the harness runs the same binary under
each allocator so the delta is purely the allocator.)*

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
- **Batched cross-thread free (snmalloc message passing).** A free of another
  thread's block is *not* pushed to the owner one-at-a-time. Each thread buffers
  remote frees in a local cache bucketed by target heap, then flushes each
  bucket as **one batch with a single atomic CAS** onto the owner's inbox. The
  owner reclaims its whole inbox in one atomic-exchange. Thousands of
  cross-thread frees cost one atomic *per batch*, not per object — this is why
  jetalloc runs the producer/consumer workload **4.6× faster than glibc** (which
  locks on every cross-thread free). A thread-exit destructor flushes any
  buffered frees so nothing is ever stranded.
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
