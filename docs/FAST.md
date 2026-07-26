# jetalloc — the crazy-tricks arsenal

Research notes: the fastest known allocator techniques, distilled from the
primary sources (tcmalloc rseq design doc, mimalloc tech report / `types.h`,
snmalloc ISMM'19 paper + internals, Linux SLUB, and CPU-arch literature), and
mapped onto what jetalloc does today and what would move its numbers next.

The through-line, from every one of these allocators: **the fastest allocator
turns allocation from a *search* into an *arithmetic commit*, and turns
synchronization from a *lock* into a *single conditional store that can be
replayed*.** Everything below is a variation on those two ideas.

---

## Tier S — the ones that actually break the speed ceiling

### 1. Restartable sequences (rseq): atomicless *per-CPU* caching  ⭐ the big one
**Source:** tcmalloc `docs/rseq.md`. This is *the* reason tcmalloc's per-CPU
mode is the fastest general allocator on Linux.

The idea: a per-**CPU** (not per-thread) slab array indexed by the logical CPU
id. The alloc fast path is a short instruction sequence registered with the
kernel via `rseq(2)`. If the thread is preempted or migrated to another CPU
*anywhere inside the sequence*, the kernel **restarts it at an abort label**
instead of letting it commit stale state. So the sequence needs **no atomics and
no locks** — it reads `current`, loads `slab[current-1]`, and the single
committing store is `hdr->current--`. Preempted mid-way? The whole thing replays
on the new CPU. Migration is the *rare* case; staying on-core is optimized.

tcmalloc's actual `Pop` (paraphrased from the doc):
```
restart: __rseq_abi.rseq_cs = &cs_Pop;      // arm the sequence
start:   slab = tcmalloc_slabs;             // cached per-CPU base (see #2)
         hdr  = &slab.header[size_class];
         cur  = hdr->current;
         ret  = *(slab + cur*8 - 8);        // the object
         next = *(slab + cur*8 - 16);
         hdr->current = cur - 1;            // ← THE COMMIT (single store)
commit:  prefetcht0(next); return ret;      // prefetch next obj for next call
```
Why it's faster than a thread-cache: **N caches for N cores, not N threads** —
far fewer caches, no false sharing, and no atomic on the push/pop at all. glibc
tcache and jetalloc's tcache both still pay a TLS load + normal stores; rseq pays
*neither an atomic nor contends*, because same-CPU is serialized by the CPU
itself.

**Cost:** must be hand-written asm (gcc/clang won't emit a correct rseq body),
x86-64 + aarch64 only, Linux ≥ 4.18. Abort handler needs the 4-byte signature
trick (an invalid opcode before the label) so a heap overflow can't redirect it.

**jetalloc plan:** add `src/jet_rseq.S` with `jet_slab_pop`/`jet_slab_push`,
register rseq per thread, keep the current thread-cache as the portable fallback
when rseq is unavailable. This is the single biggest remaining win on the
threaded benchmark (where we're at 0.90× glibc).

### 2. Per-CPU slab-pointer caching via `cpu_id_start` overlap  (crazy pointer hack)
**Source:** same doc, "Current CPU Slabs Pointer Caching."

Computing "the slab base for my current CPU" every call is expensive (vCPU +
variable shift). tcmalloc caches the current-CPU slab pointer in TLS **and
overlaps its top 4 bytes with `__rseq_abi.cpu_id_start`**. When the kernel
migrates the thread it *overwrites* `cpu_id_start` with the new CPU number — 
which corrupts the cached pointer's high bits. They set the pointer's top bit
(bit 63) so a valid cached pointer is recognizable; a real CPU number (<2^31)
never has bit 63 set. So the entire "is my cached CPU pointer still valid?" check
collapses to:
```
slabs = load(__rseq_abi - 4);
if ((slabs & (1<<63)) == 0) goto slowpath;   // kernel clobbered it → migrated
slabs &= ~(1<<63);
```
The kernel migration handler *is* the cache-invalidation signal, for free. No
comparison against a stored CPU id, no branch to recompute. Beautiful.

### 3. Combined bump-pointer + free-list in ONE word  (snmalloc)
**Source:** snmalloc ISMM'19 — "a novel bump pointer-free list data structure
with which just **64 bits of metadata** are sufficient for each 64 KiB slab."

Most allocators track a slab with a freelist head *and* a bump cursor *and* a
count (jetalloc uses ~5 fields). snmalloc encodes the slab's entire allocation
state so a 64 KiB slab needs only 64 bits of metadata, kept in a **pagemap**
(a flat array indexed by address bits) — *not* inline in the slab. Two wins:
- **Zero metadata in the object pages** → slab data pages hold pure user data,
  denser cache/TLB, and the metadata array stays hot separately.
- The bump region and freelist are unified so alloc is one path, not "try
  freelist else bump."

jetalloc already did the bump-vs-freethread split (commit 3643933); snmalloc's
lesson is to go further: **move page headers OUT of the pages into a side
pagemap**, so a slab page is 100% user bytes.

---

## Tier A — big, portable, no kernel magic

### 4. Free-list *multi*-sharding (mimalloc's "big idea")
**Source:** mimalloc readme + tech report. jetalloc already has the two-list
version (`local_free` + atomic `thread_free`). mimalloc's full scheme has
*three* per-page lists: `free` (alloc pops here), `local_free` (owner frees push
here), and a `thread_free` atomic (remote frees, single CAS). The `free` list is
only refilled from `local_free` at a "page collect" — this **separates the alloc
cursor from the free cursor entirely**, so a free never touches the list the
allocator is currently popping (no write-write cache ping-pong on one line). It
also enables a *deferred* free that batches the collect. jetalloc's `alloc_free`
vs `local_free` is exactly this; the missing bit is the periodic generic-collect
heartbeat.

### 5. Sized-class → index by one multiply-shift, no table
**Source:** tcmalloc/mimalloc size-class math. jetalloc uses a 2 KiB lookup
table. The faster trick used in production: for size `n`, the class is derived by
a couple of `lzcnt` + shift ops (a "log-linear" mapping) so there's **no memory
load at all** on the size→class step — pure ALU. Removes a potential L1 miss from
the hottest computation. Worth trying vs the table (table is 2 KiB, stays hot,
but ALU-only is a guaranteed 0 loads).

### 6. Radix-tree / flat pagemap for `owns()` and metadata  (snmalloc, tcmalloc)
**Source:** snmalloc `FlatPagemap`, tcmalloc pagemap. jetalloc's `jet_owns()`
currently sanity-checks page-header fields — probabilistic. A flat pagemap (one
byte or word per 64 KiB of VA, indexed by `addr >> 16`) gives an **exact O(1)
owner/sizeclass lookup with zero header touch**, which also makes the interposer
100% safe (no false "owns"). On a 48-bit VA a `addr>>16` byte-map is 4 GB *of
virtual* address but only the touched pages commit — pairs perfectly with #12.

### 7. Two-level "transfer cache" / magazines between tcache and central
**Source:** tcmalloc transfer cache, Hoard magazines. When the thread cache
under/overflows, move a **whole batch** (e.g. 32 objects) to/from the central
heap in one locked op, not one object at a time. jetalloc's `tcache_flush`
already batches on overflow; the missing half is **batch *refill*** on
underflow (currently `alloc_small` fills one block per miss). Batching the
refill amortizes the central spinlock across 32 allocs — directly targets the
small-fixed regression.

---

## Tier B — cache / TLB / NUMA microarchitecture

### 8. Huge pages for the slab spans  (done ✓, keep pushing)
`MADV_HUGEPAGE` on 4 MiB spans is in (commit 557e827). Next: **explicit 2 MiB
`MAP_HUGETLB`** for arenas on servers → guaranteed huge pages, ~zero TLB misses
on the slab region. tcmalloc calls this its single biggest server win.

### 9. Cache coloring / page coloring
**Source:** Linux SLUB slab coloring, Wikipedia cache-coloring. Consecutive
same-class slabs start their data at slightly **different offsets** (the "color")
so hot objects from different slabs don't all map to the *same* L1/L2 cache set.
Without it, object #0 of every 64 KiB slab collides in the cache. SLUB literally
adds a per-slab color offset. Cheap: offset the `bump`/`data` start by
`(slab_index * 64) % free_bytes`.

### 10. Cache-line-aligned, false-sharing-free remote queues
**Source:** snmalloc `RemoteAllocator` — `front` and `back` of the message queue
are on **separate cache lines** (`alignas(CACHELINE_SIZE)`), because producer and
consumer threads touch them independently. jetalloc's `thread_free` sits inside
the page header sharing a line with `owner`/`block_size` that the owner reads on
every alloc → the remote-freeing thread's CAS invalidates the owner's line
(false sharing). Splitting `thread_free` onto its own line is a direct threaded
win.

### 11. NUMA-local arenas + first-touch
Bind each per-CPU (or per-socket) arena's pages to the local NUMA node
(`mbind`/`set_mempolicy`, or just first-touch on the owning core so the default
first-touch policy places them local). Cross-socket DRAM is ~2× latency; a
server allocator that hands out remote-node memory silently halves bandwidth.

---

## Tier C — virtual-memory sorcery

### 12. Reserve enormous VA, commit on touch  (roadmap L14)
`mmap(PROT_NONE, 16 TiB)` reserves address space for ~free (no RAM until
touched). Carve all arenas from that one contiguous reservation → **ownership by
address becomes a single subtract-and-shift** (#6 pagemap gets trivial and
dense), zero per-span `mmap` syscalls, and the whole heap is one range for the
pagemap. mimalloc/snmalloc both reserve big and commit lazily.

### 13. Lazy decommit with `MADV_FREE`, not `MADV_DONTNEED`
**Source:** mimalloc "eager page purging", BSD/Linux `MADV_FREE`. When a slab
empties, don't `munmap` (expensive re-fault later) — `MADV_FREE` tells the kernel
"you *may* reclaim these pages, but if I touch them again before you do, cancel
that." Reused-before-reclaim costs **zero** page faults, vs `MADV_DONTNEED`/unmap
which guarantees a fault + zero-fill on the next touch. This is the right fix for
jetalloc's stub `jet_trim()`.

### 14. Never write freed memory you're about to reuse
snmalloc/mimalloc keep the freelist link in the object's *first word* (jetalloc
does this), but the deeper trick is **don't `memset` on free and don't re-zero on
alloc from fresh mmap** — mmap memory is already zero, so `calloc` from a fresh
bump region needs no clear. jetalloc already skips zeroing large (mmap) allocs;
extend it: track a per-page "still pristine (bump-only, never freed)" bit so
`calloc` of a bump block also skips the memset.

---

## Tier D — security tricks that are ~free and worth it

### 15. Encrypted / signed free-list pointers
**Source:** snmalloc `mitigations.h`, secure-allocator writeups. XOR each
freelist `next` pointer with a per-slab random key (and a check value). A heap
overflow that smashes a `next` pointer now yields a garbage address that fails a
cheap validity check → "Heap corruption detected" instead of an exploit
primitive. Cost is ~1 xor on the free path. mimalloc-secure and snmalloc both do
this for ~10% or less overhead; jetalloc could offer it under `-DJET_SECURE`.

---

## Priority order for jetalloc (speed-per-effort)

1. **#10 false-sharing split** of `thread_free` onto its own cache line — tiny
   diff, direct threaded win.
2. **#7 batch refill** the tcache on underflow — fixes small-fixed, amortizes the
   central lock.
3. **#12 + #6** reserve-big-VA + flat pagemap — makes `owns()` exact and unlocks
   dense ownership; foundational for the rest.
4. **#13 `MADV_FREE`** decommit — real memory return, the missing `jet_trim`.
5. **#1 + #2 rseq per-CPU fast path** (asm) — the ceiling-breaker; biggest and
   hardest. Do it once the above are in and measured.
6. **#9 cache coloring**, **#5 ALU size-class**, **#15 secure lists** — polish.

Every number here is traceable to a shipping allocator's source. None of it is
speculative; the only open question is how much each moves *jetalloc's* numbers
on *your* CPU, which is why each lands as its own commit with a before/after
bench.
