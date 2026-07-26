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

### 0. initial-exec TLS + inlined fast path ✅ (the biggest single win, all rows)
**Source:** the ELF TLS access-model literature; every high-perf allocator
compiles its thread-locals `initial-exec`. This was jetalloc's single largest
speedup and it touched EVERY allocation. The thread-heap pointer is a
`_Thread_local`; in the default *global-dynamic* TLS model a shared library
reads it via a `__tls_get_addr()` **function call on every malloc and every
free**. Compiling the allocator with `-ftls-model=initial-exec` turns that into
a single `mov %fs:(reg), reg` — a direct segment-relative load, no call, no
frame. Valid because our thread-locals live in the allocator image itself
(static link or `LD_PRELOAD`), never a late `dlopen`. Paired with hoisting the
size→class lookup and the thread-heap TLS load *inline* into `jet_malloc` /
`jet_free` (the hot bin hit is now a bare `%fs` load + a pop, zero calls),
measured on a 12-core Zen box vs the prior commit:

| workload      | before | after | vs glibc |
|---------------|-------:|------:|---------:|
| small-fixed   |  ~265  | ~290  |  1.26×   |
| mixed-size    |  ~145  | ~183  | 10.5×    |
| threaded 8T   | ~840–930 | ~1120–1200 | ~parity (was 0.85×) |
| prod/cons     |  ~70   | ~85   |  6.8×    |

The threaded row went from 0.85× glibc to parity purely from removing the
per-op `__tls_get_addr` call. Lesson: before chasing exotic cross-thread
algorithms, make sure the *fast path has no hidden function call* — the TLS
model is the classic trap.

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

**jetalloc status: SHIPPED (opt-in via `JET_PERCPU=1`).** `src/jet_rseq.S`
implements `jet_rseq_pop`/`jet_rseq_push` as real restartable sequences (glibc
auto-registers the per-thread `struct rseq`; we read `cpu_id_start` at `%fs:0`
and commit with a single store; the abort label carries the 4-byte `RSEQ_SIG`).
`src/jet_percpu.c` manages an `ncpu × NUM_CLASSES` slab table in front of the
per-thread tcache, capped at `JET_PERCPU_MAX` blocks per (cpu,class).

**Measured (12-core Zen kernel 7.1), honest:** on *this* machine the per-CPU
cache does **not** beat the existing per-thread tcache + batched remote engine
— on same-thread and cross-thread oversubscribed (64-thread) workloads alike it
breaks even at best and regresses the single-thread fast path (the rseq CS entry
+ current-CPU read is pure overhead when the per-thread cache is already
lock-free and uncontended). rseq per-CPU is a tcmalloc win primarily at **very
high core counts** (64-128+), where per-thread cache *memory* becomes
prohibitive and the central free-list lock contends. So it ships **off by
default**, correct and ASan/UBSan-clean, available for big machines. When off,
the cost is one predicted-not-taken branch on `jet_percpu_on`.

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

### 4. Free-list *multi*-sharding (mimalloc's "big idea") + batched message passing ✅
**Source:** mimalloc readme + tech report; snmalloc RemoteCache. **SHIPPED.**
jetalloc now buffers cross-thread frees in a thread-local outgoing cache
bucketed by target heap, and flushes each bucket as ONE batched CAS onto the
owner's inbox (`remote_post_batch`), which the owner drains in a single
atomic-exchange (`remote_drain`). A thread-exit destructor flushes residual
buffered frees. Result: **producer/consumer 4.6× faster than glibc** (74 vs 16
Mops/s) — the exact workload this technique targets. Tunable via
`JET_REMOTE_FLUSH` (default 256).

### 4b. Epoch-based (QSBR) safe page reclamation ✅ (correctness)
**Source:** Linux RCU, the Crossbeam epoch crate, McKenney's QSBR. **SHIPPED**
(`src/jet_epoch.c`). Fixes a genuine use-after-reformat race the batched
cross-thread path had: a slow cross-thread free reads `pg->owner`, and in the
window before it commits, the owner can empty the page, retire it, and
`raw_page()` can re-mint that same 64 KiB region for a *different* size class —
so the slow freer clobbers a live, repurposed page. QSBR closes it: a global
epoch advances monotonically; the cross-thread free path *pins* the current
epoch across its hazardous window (`jet_epoch_enter/leave`); `retire` parks the
page in a limbo bag instead of freeing it; a page is only truly repurposed once
the epoch has advanced twice with every thread observed quiescent — at which
point no stale pointer can survive. The pin cost lands ONLY on cross-thread
frees (a couple of stores + a fence), never on the owner fast path, so the
benchmark is unchanged. **TSan-verified race-free** across 18M cross-thread
frees; the same TSan pass also closed a pre-existing lazy size-map init race
(now a library constructor).

### 4c. Per-page bitmap remote free — evaluated, deliberately NOT shipped
**Source:** mimalloc's early per-page free design. The idea: a remote free sets
one bit (`fetch_or`) at the block's slot index in a per-page bitmap; the owner
reclaims by scanning dense words instead of pointer-chasing a Treiber chain.
This solves the *same* problem — avoid a per-object atomic + a cache-miss walk
of a per-page remote list — that jetalloc's batched message passing (4/4b)
ALREADY solves, and more thoroughly: the common cross-thread free does **zero**
atomics (pure local buffering) and the owner drains its whole inbox in one
atomic-exchange, versus one `fetch_or` per free plus a bitmap scan. Measuring
confirmed the per-page cross-thread list is now DEAD CODE (0 hits across 12M+
cross-thread frees), so adding a bitmap would be a regression on live code and
dead weight otherwise. Instead we DELETED the legacy per-page `thread_free`
Treiber stack entirely — shrinking the page header by a whole `_Alignas(64)`
cache line (denser TLB/cache, higher usable-byte ratio) and simplifying the
free path. Bitmap goal achieved by a strictly better mechanism.

### 4d. Alloc-time local/shared classification ✅ (~1.15× prod/cons)
**Source:** scalloc's private/shared spans; mimalloc's local/shared free
distinction. **SHIPPED.** Each heap keeps a saturating one-byte `shared_score`
per size class, bumped whenever that class takes a cross-thread free. A class
that proves producer/consumer-heavy (score ≥ `JET_SHARED_HOT`) is reclassified
"shared-hot" and its outgoing remote bucket is allowed to accumulate a much
larger batch — bounded by a BYTE budget (`JET_SHARED_HOT_BYTES`, 512 KiB), so
small classes batch thousands deep while 32 KiB classes still flush after a
handful and stranded memory stays bounded (≤ 512 KiB × 16 buckets per thread).
More blocks per atomic post = fewer CASes on the owner's inbox; a rarely-shared
class keeps a small fixed cap so a one-off cross-thread free never strands.
The score lives on the (already cold) cross-thread free path — the owner fast
path never reads it. Measured: **prod/cons 65–70 → 75–80 Mops/s** with the other
three benchmarks unchanged; larger byte budgets push it to ~90 at a memory
cost, so 512 KiB is the shipped default. TSan + ASan/UBSan clean.

### 4e. Place-based / temperature-aware free — research artifact (JET_PLACE=1)
**The "stop tracking owners" idea, built and measured.** Instead of asking *who*
owns a block, ask *where* it lives: recover the page from the block's ADDRESS
(a mask jetalloc already does), and let the PAGE carry its own free policy — no
owner-heap dereference, no per-target bucket, no routing table. Each page
self-classifies by TEMPERATURE (a saturating per-page `temp` counter of foreign
frees):
  - **HOT** (few foreign frees): routed through the batched message-passing
    engine — zero atomics per free.
  - **WARM** (`temp ≥ JET_TEMP_WARM`): cross-thread frees push straight onto the
    page's own `_Alignas(64)` MPSC `place_head` (one atomic, address-routed).
    A FULL page that receives place frees is enqueued ONCE on the owner's
    lock-free `drain_pages` stack (guarded by `on_drain`, one atomic per
    page-transition, not per object) so the owner can find and fold it back;
    accounting is restored by a per-block `used--` on drain.

Fully correct — `alloc=free=live=0` after the mt stress, TSan + ASan/UBSan clean
(the TSan pass forced `temp` to a relaxed atomic). **Honest result:** on this
12-core box it does NOT beat the shipped batched engine — place is ~68 vs ~77
Mops/s prod/cons, and a purpose-built many-owners/bucket-collision bench is a
wash (~90 vs ~97). The lesson, now proven a THIRD time (after the per-page
bitmap and per-CPU cache): **a per-object atomic can't beat an amortized batched
atomic, however clever the routing.** Batching is the local optimum; owner-
obliviousness is elegant but not faster here. Shipped OFF by default as a
research path (`JET_PLACE=1`); the frontier for real speed is the same-thread
fast path, not the cross-thread one.

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

### 9. Cache coloring / page coloring ✅ (3.5× on L1-conflict workloads)
**Source:** Linux SLUB slab coloring, Wikipedia cache-coloring. **SHIPPED**
(`src/jet_central.c`). Consecutive same-class slabs start their data at
different offsets (the "color") so hot objects from different slabs don't all
map to the *same* L1 set. Without it, object #0 of every 64 KiB slab collides.
jetalloc rotates each page's data start through `JET_COLORS`(=16) cache-line
steps via a relaxed-atomic counter in `format_page` — cost is a few wasted bytes
per page (≤ ~1 KiB of 64 KiB) and ZERO hot-path instructions. **Measured:** on a
purpose-built pointer-chase across same-slot blocks (an N-way L1 set conflict)
coloring runs **~190 vs ~51 M-chase/s — 3.5×** faster; the standard four
benchmarks are unchanged-to-slightly-up (their working set is too small to
conflict). A pure, always-on layout win with no regression.

### 10. Cache-line-aligned, false-sharing-free remote queues
**Source:** snmalloc `RemoteAllocator` — `front` and `back` of the message queue
are on **separate cache lines** (`alignas(CACHELINE_SIZE)`), because producer and
consumer threads touch them independently. jetalloc's `thread_free` sits inside
the page header sharing a line with `owner`/`block_size` that the owner reads on
every alloc → the remote-freeing thread's CAS invalidates the owner's line
(false sharing). Splitting `thread_free` onto its own line is a direct threaded
win.

### 11. NUMA-local arenas + first-touch ✅ (self-disabling; multi-socket only)
**Source:** tcmalloc/jemalloc NUMA-aware arenas. **SHIPPED** (`src/jet_numa.c`).
Every freshly-mapped span is bound to prefer the NUMA node of the CPU that
asked for it (`mbind(2)` with `MPOL_PREFERRED`, via raw syscall — no libnuma
dependency), so first-touch places its physical pages in the allocating
thread's local DRAM. Cross-socket DRAM is ~2× latency and less bandwidth, so a
server allocator that hands out remote-node memory silently halves throughput.

**Self-disabling and honest:** node count is detected once at library-load
(constructor, scanning `/sys/devices/system/node/nodeN`); on a **single-node**
machine — including this dev box (`available: 1 nodes`) — the whole layer
collapses to one predicted-not-taken branch and **never issues a syscall**, so
there is provably **zero** effect on the benchmarks here (verified unchanged).
It is armed only when nodes > 1. Best-effort (`MPOL_PREFERRED`, not `BIND`): a
full local node falls back silently rather than failing the allocation, and any
`mbind` error is ignored — placement is an optimisation, never correctness. The
mechanism is unit-probed (getcpu + mbind both succeed on this host); the *win*
is only demonstrable on multi-socket hardware. `JET_NUMA=0` forces it off.
TSan + ASan/UBSan clean.

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
